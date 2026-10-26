#!/usr/bin/env python3
"""Static verifier for UniXNU boot images (Android boot header + BootShim + UEFI FD + DTB).

UniXNU's `./unixnu bootimg` produces an Android boot image whose "kernel" is:

    gzip( BootShim.AArch64ImageHeader || UEFI firmware volume ) || compiled DTB

This script unpacks the Android header (v0..v4), gunzips the kernel payload, splits the
BootShim from the firmware volume, validates the AArch64 Image header fields the shim
inherits (magic "ARM\\x64", text_offset = FD base, image_size = shim + FD), checks the
FV header signature/length, checks the appended DTB magic/totalsize, and prints offsets,
sizes and SHA-256 for every part. No device or UEFI runtime is needed.

Exit status: 0 = all checks passed, 1 = at least one check failed, 2 = unreadable input.

Usage:
    python3 verify-bootimg.py dist/boot.img
    python3 verify-bootimg.py dist/boot.img --json dist/boot.json
    python3 verify-bootimg.py dist/boot.img --fd-base 0x80000000 --fd-size 0x400000
    python3 verify-bootimg.py dist/boot.img --expect-header-version 0 --expect-page-size 4096
"""
import argparse
import gzip
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

PAGE_SIZES = (2048, 4096, 8192, 16384, 32768, 65536)
FV_SIGNATURE = b'_FVH'
DTB_MAGIC = 0xD00DFEED
ARM64_IMAGE_MAGIC = b'ARM\x64'


class Report:
    def __init__(self):
        self.checks = []       # (name, ok, detail)
        self.info = {}

    def check(self, name, ok, detail=''):
        self.checks.append((name, bool(ok), detail))
        return ok

    def fail(self, name, detail):
        return self.check(name, False, detail)

    @property
    def failed(self):
        return [item for item in self.checks if not item[1]]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def u64(data, offset):
    return struct.unpack_from('<Q', data, offset)[0]


def parse_android_header(data, report):
    """Return dict describing the Android boot image layout (header v0..v4)."""
    if len(data) < 44:
        report.fail('android header', f'image is only {len(data)} bytes')
        return None
    if data[:8] != b'ANDROID!':
        report.fail('android magic', f'expected "ANDROID!", got {data[:8]!r}')
        return None
    report.check('android magic', True, '"ANDROID!"')
    version = u32(data, 40)
    header_size = u32(data, 20)
    page_size = u32(data, 36)
    # v3/v4 put header_size=4096 at offset 20; v0..v2 keep second_size there (normally 0)
    # and a valid page_size at offset 36 (v3/v4 keep 0 there: reserved[3]).
    if version in (3, 4) or (version in (0, 1, 2) and header_size == 4096 and page_size not in PAGE_SIZES):
        if version not in (3, 4):
            report.fail('android header version', f'offset 40 says {version}, layout looks like v3/v4')
            return None
        if header_size != 4096:
            report.fail('android header_size', f'v{version} header_size is {header_size}, expected 4096')
            return None
        if page_size not in (0,) + PAGE_SIZES:
            report.fail('android page_size', f'reserved[3] is {page_size}, expected 0')
        kernel_size = u32(data, 8)
        ramdisk_size = u32(data, 12)
        os_version = u32(data, 16)
        cmdline = data[44:44 + 1536].split(b'\0', 1)[0]
        signature_size = u32(data, 1580) if version == 4 and len(data) >= 1584 else 0
        kernel_offset = 4096 + (4096 if version == 4 else 0)
        ramdisk_offset = kernel_offset + -(-kernel_size // 4096) * 4096
        layout = {
            'version': version, 'header_size': header_size, 'page_size': 4096,
            'kernel_size': kernel_size, 'kernel_offset': kernel_offset,
            'ramdisk_size': ramdisk_size, 'ramdisk_offset': ramdisk_offset,
            'os_version': os_version, 'cmdline': cmdline.decode('utf-8', 'replace'),
            'signature_size': signature_size, 'second_size': 0,
            'kernel_addr': None, 'ramdisk_addr': None, 'tags_addr': None,
        }
        report.check('android header v3/v4', True,
                     f'v{version} header_size={header_size} kernel@0x{kernel_offset:x} '
                     f'ramdisk@0x{ramdisk_offset:x} signature={signature_size}')
        return layout
    # v0..v2
    if version not in (0, 1, 2):
        report.fail('android header version', f'unsupported header_version {version}')
        return None
    minimum = {0: 1632, 1: 1648, 2: 1660}[version]
    if len(data) < minimum:
        report.fail('android header size', f'v{version} header needs {minimum} bytes, image is {len(data)}')
        return None
    kernel_size = u32(data, 8)
    kernel_addr = u32(data, 12)
    ramdisk_size = u32(data, 16)
    ramdisk_addr = u32(data, 20)
    second_size = u32(data, 24)
    tags_addr = u32(data, 32)
    page_size = u32(data, 36)
    os_version = u32(data, 44)
    name = data[48:64].split(b'\0', 1)[0]
    cmdline = data[64:64 + 512].split(b'\0', 1)[0]
    extra = data[608:608 + 1024].split(b'\0', 1)[0]
    if page_size not in PAGE_SIZES:
        report.fail('android page_size', f'{page_size} is not a plausible page size {PAGE_SIZES}')
        return None
    if version >= 1:
        dtbo_size = u32(data, 1632)
        dtbo_offset = u64(data, 1636)
        hdr_size = u32(data, 1644)
    else:
        dtbo_size = dtbo_offset = hdr_size = 0
    if version >= 2:
        dtb_size = u32(data, 1648)
        dtb_addr = u64(data, 1652)
    else:
        dtb_size = dtb_addr = 0
    # Layout order written by mkbootimg.py: header page, kernel, ramdisk, second,
    # recovery_dtbo, dtb - each starting on a page boundary.
    def pages(count):
        return -(-count // page_size) * page_size
    dtb_offset_in_image = (page_size + pages(kernel_size) + pages(ramdisk_size)
                           + pages(second_size) + pages(dtbo_size))
    layout = {
        'version': version, 'header_size': hdr_size or 1632 + (16 if version >= 1 else 0) + (12 if version >= 2 else 0),
        'page_size': page_size, 'kernel_size': kernel_size,
        'kernel_offset': page_size, 'kernel_addr': kernel_addr,
        'ramdisk_size': ramdisk_size,
        'ramdisk_offset': page_size + -(-kernel_size // page_size) * page_size,
        'ramdisk_addr': ramdisk_addr, 'second_size': second_size, 'tags_addr': tags_addr,
        'os_version': os_version, 'cmdline': (cmdline + b' ' + extra).decode('utf-8', 'replace').strip(),
        'name': name.decode('utf-8', 'replace'), 'dtbo_size': dtbo_size,
        'dtbo_offset': dtbo_offset, 'dtb_size': dtb_size, 'dtb_addr': dtb_addr,
        'dtb_offset_in_image': dtb_offset_in_image if dtb_size else 0,
        'signature_size': 0,
    }
    report.check('android header v0-v2', True,
                 f'v{version} page={page_size} kernel_size={kernel_size} '
                 f'ramdisk_size={ramdisk_size} kernel_addr=0x{kernel_addr:x} tags=0x{tags_addr:x}')
    return layout


def parse_bootshim(shim, fd_size, fd_base, report, fd_total=None):
    """Validate the AArch64 Image header the BootShim carries in front of the FD."""
    if len(shim) < 64:
        report.fail('bootshim header', f'shim is only {len(shim)} bytes, shorter than a 64-byte Image header')
        return
    text_offset = u64(shim, 8)
    image_size = u64(shim, 16)
    flags = u64(shim, 24)
    magic = shim[56:60]
    report.check('bootshim ARM64 magic', magic == ARM64_IMAGE_MAGIC,
                 f'bytes 56..60 = {magic!r} (expected b"ARM\\\\x64")')
    if fd_base is not None:
        report.check('bootshim text_offset == FD base', text_offset == fd_base,
                     f'text_offset=0x{text_offset:x} fd_base=0x{fd_base:x}')
    else:
        report.info['bootshim_text_offset'] = hex(text_offset)
        report.check('bootshim text_offset', text_offset != 0, f'text_offset=0x{text_offset:x} (no --fd-base given)')
    expect_image = len(shim) + (fd_size if fd_size is not None else 0)
    if fd_size is not None:
        report.check('bootshim image_size == shim + FD', image_size == expect_image,
                     f'image_size=0x{image_size:x} shim=0x{len(shim):x} fd_size=0x{fd_size:x} '
                     f'expected=0x{expect_image:x}')
    else:
        report.info['bootshim_image_size'] = hex(image_size)
        report.check('bootshim image_size', image_size > len(shim),
                     f'image_size=0x{image_size:x} implies FD of 0x{image_size - len(shim):x} bytes')
    report.info.update({'bootshim_bytes': len(shim), 'bootshim_sha256': sha256(shim),
                        'bootshim_flags': hex(flags), 'bootshim_code0': hex(u32(shim, 0)),
                        'bootshim_code1': hex(u32(shim, 4))})


def find_fd_start(combined, shim_min=64, scan_limit=0x20000):
    """Locate the UEFI firmware volume: a valid FV header starts 40 bytes before its _FVH signature."""
    limit = min(len(combined), scan_limit)
    for start in range(0, limit):
        if combined[start + 40:start + 44] != FV_SIGNATURE:
            continue
        if start + 40 + 4 > len(combined):
            continue
        fv_length = u64(combined, start + 32)
        if fv_length == len(combined) - start and fv_length > 0x1000:
            return start, fv_length
    # Fall back to the first _FVH hit even if the length field is odd (report it as a failure).
    index = combined.find(FV_SIGNATURE)
    if index >= 40:
        return index - 40, u64(combined, index - 40 + 32)
    return None, None


def parse_fd(fd, report, expected_size=None, fd_base=None):
    ok = True
    if len(fd) < 0x48:
        return report.fail('firmware volume', f'FD is only {len(fd)} bytes')
    ok &= report.check('firmware volume signature', fd[40:44] == FV_SIGNATURE,
                       f'offset 40 = {fd[40:44]!r} (expected b"_FVH")')
    fv_length = u64(fd, 32)
    ok &= report.check('firmware volume length field', fv_length == len(fd),
                       f'FV header length=0x{fv_length:x} actual=0x{len(fd):x}')
    if expected_size is not None:
        ok &= report.check('firmware volume size matches config', len(fd) == expected_size,
                           f'FD is 0x{len(fd):x} bytes, expected 0x{expected_size:x}')
    guid = fd[16:32].hex()
    report.info.update({'fd_bytes': len(fd), 'fd_sha256': sha256(fd), 'fd_fv_length': hex(fv_length),
                        'fd_filesystem_guid': guid,
                        'fd_zero_vector': fd[:16] == b'\0' * 16})
    if fd_base is not None:
        report.info['fd_base'] = hex(fd_base)
    return ok


def parse_dtb(dtb, report):
    if len(dtb) < 8:
        return report.fail('device tree', f'appended DTB is only {len(dtb)} bytes')
    magic = struct.unpack_from('>I', dtb, 0)[0]
    totalsize = struct.unpack_from('>I', dtb, 4)[0]
    ok = report.check('device tree magic', magic == DTB_MAGIC,
                      f'0x{magic:08x} (expected 0x{DTB_MAGIC:08x})')
    ok &= report.check('device tree totalsize', totalsize == len(dtb),
                       f'totalsize=0x{totalsize:x} appended bytes=0x{len(dtb):x}')
    report.info.update({'dtb_bytes': len(dtb), 'dtb_sha256': sha256(dtb),
                        'dtb_totalsize': hex(totalsize)})
    if ok:
        off_struct = struct.unpack_from('>I', dtb, 8)[0]
        off_strings = struct.unpack_from('>I', dtb, 12)[0]
        # FDT header: off_mem_rsvmap at 16, version at 20, ..., size_dt_strings at
        # 32, size_dt_struct at 36.
        size_struct = struct.unpack_from('>I', dtb, 36)[0]
        version = struct.unpack_from('>I', dtb, 20)[0]
        report.info.update({'dtb_version': version, 'dtb_off_struct': hex(off_struct),
                            'dtb_off_strings': hex(off_strings)})
        # Minimal FDT walk: pull the root node's model/compatible out of the
        # structure block (property values live there; the strings block only
        # holds property names).
        try:
            struct_block = dtb[off_struct:off_struct + size_struct]
            strings = dtb[off_strings:]
            depth = 0
            cursor = 0
            while cursor + 4 <= len(struct_block):
                token = struct.unpack_from('>I', struct_block, cursor)[0]
                cursor += 4
                if token == 1:      # FDT_BEGIN_NODE
                    depth += 1
                    cursor += (struct_block.index(b'\0', cursor) - cursor) + 1
                    cursor += -cursor % 4
                elif token == 2:    # FDT_END_NODE
                    depth -= 1
                elif token == 3:    # FDT_PROP
                    length, name_offset = struct.unpack_from('>II', struct_block, cursor)
                    cursor += 8
                    value = struct_block[cursor:cursor + length]
                    cursor += length + (-length % 4)
                    if depth == 1:
                        name = strings[name_offset:strings.index(b'\0', name_offset)].decode('ascii', 'replace')
                        if name in ('model', 'compatible'):
                            text = value.rstrip(b'\0').decode('utf-8', 'replace')
                            report.info[f'dtb_{name}'] = ' | '.join(
                                part for part in text.split('\0') if part)
                elif token == 4:    # FDT_NOP
                    continue
                elif token == 9:    # FDT_END
                    break
                else:
                    break
        except (ValueError, struct.error):  # noqa: BLE001 - best-effort summary only
            pass
    return ok


def verify(path, fd_base=None, fd_size=None, expect_header_version=None, expect_page_size=None):
    report = Report()
    data = path.read_bytes()
    report.info.update({'image': str(path), 'image_bytes': len(data), 'image_sha256': sha256(data)})
    layout = parse_android_header(data, report)
    if layout is None:
        return report
    if expect_header_version is not None:
        report.check('expected header version', layout['version'] == expect_header_version,
                     f'got v{layout["version"]}, expected v{expect_header_version}')
    if expect_page_size is not None:
        report.check('expected page size', layout['page_size'] == expect_page_size,
                     f'got {layout["page_size"]}, expected {expect_page_size}')
    report.info['android'] = {k: v for k, v in layout.items() if not isinstance(v, bytes)}
    page = layout['page_size']
    kernel = data[layout['kernel_offset']:layout['kernel_offset'] + layout['kernel_size']]
    report.check('kernel region inside image',
                 len(kernel) == layout['kernel_size'] and
                 layout['kernel_offset'] + layout['kernel_size'] <= len(data),
                 f'kernel bytes 0x{layout["kernel_offset"]:x}+0x{layout["kernel_size"]:x} '
                 f'of image 0x{len(data):x}')
    if layout['ramdisk_size']:
        ramdisk = data[layout['ramdisk_offset']:layout['ramdisk_offset'] + layout['ramdisk_size']]
        report.check('ramdisk region inside image',
                     len(ramdisk) == layout['ramdisk_size'],
                     f'ramdisk bytes 0x{layout["ramdisk_offset"]:x}+0x{layout["ramdisk_size"]:x}')
        report.info.update({'ramdisk_bytes': layout['ramdisk_size'], 'ramdisk_sha256': sha256(ramdisk)})
    else:
        report.info['ramdisk_bytes'] = 0

    # Kernel encoding: UniXNU's native layout is gzip(BootShim+FD) || DTB; the
    # stock device layout is a RAW BootShim+FD image with the DTB in the header
    # v2 fields. Accept both, detect by the gzip magic.
    if kernel[:2] == b'\x1f\x8b':
        encoding = 'gzip'
        decomp = zlib.decompressobj(16 + zlib.MAX_WBITS)
        try:
            combined = decomp.decompress(kernel)
        except zlib.error as error:
            report.fail('kernel gzip', f'decompression failed: {error}')
            return report
        report.check('kernel gzip stream', decomp.eof,
                     f'gzip stream ends after 0x{len(kernel) - len(decomp.unused_data):x} bytes')
        appended = decomp.unused_data
    else:
        encoding = 'raw'
        combined = kernel
        appended = b''
        report.check('kernel is a raw Image (not gzip)', True,
                     f'raw kernel of 0x{len(kernel):x} bytes (BootShim + FD)')
    report.info['kernel_encoding'] = encoding
    # Android header v2 can carry the device tree in its own fields instead of
    # appending it to the kernel (stock eebbk A2 boot.bin does exactly that).
    header_tree = b''
    if layout.get('dtb_size'):
        start = layout['dtb_offset_in_image']
        header_tree = data[start:start + layout['dtb_size']]
        report.check('header dtb region inside image',
                     len(header_tree) == layout['dtb_size'],
                     f'dtb bytes 0x{start:x}+0x{layout["dtb_size"]:x} of image 0x{len(data):x}')
        report.info['header_dtb_addr'] = hex(layout['dtb_addr'])
    report.info.update({'kernel_bytes': len(kernel), 'kernel_sha256': sha256(kernel),
                        'combined_bytes': len(combined), 'combined_sha256': sha256(combined),
                        'appended_bytes': len(appended), 'header_dtb_bytes': len(header_tree)})
    if not combined:
        report.fail('combined payload', 'gzip decompressed to zero bytes')
        return report
    fd_start, fv_length = find_fd_start(combined)
    if fd_start is None:
        report.fail('firmware volume start', 'no _FVH signature in the first 128 KiB of the payload')
        return report
    report.check('firmware volume found', True,
                 f'BootShim = first 0x{fd_start:x} bytes, FD starts at 0x{fd_start:x}')
    parse_bootshim(combined[:fd_start], fv_length, fd_base, report)
    fd = combined[fd_start:]
    parse_fd(fd, report, fd_size)
    if appended:
        parse_dtb(appended, report)
    if header_tree:
        parse_dtb(header_tree, report)
    if not appended and not header_tree:
        report.fail('device tree', 'no DTB appended after the gzip stream and no header dtb_size')
    tree_bytes = len(appended) or len(header_tree)
    dtb_in_image = (layout['kernel_offset'] + len(kernel) - len(appended)) if appended \
        else layout.get('dtb_offset_in_image')
    report.info['layout'] = {
        'android_header_bytes': layout['header_size'],
        'kernel_offset': hex(layout['kernel_offset']),
        'kernel_bytes': layout['kernel_size'],
        'ramdisk_offset': hex(layout['ramdisk_offset']) if layout['ramdisk_size'] else None,
        'bootshim_offset': hex(layout['kernel_offset']),
        'bootshim_bytes': fd_start,
        # The FD is inside the gzip stream, so it has no direct image offset;
        # this is its offset inside the decompressed BootShim + FD payload.
        'fd_offset_in_payload': hex(fd_start),
        'fd_bytes': len(fd),
        'dtb_offset_in_image': hex(dtb_in_image) if dtb_in_image is not None else None,
        'dtb_bytes': tree_bytes,
        'dtb_source': 'appended after gzip stream' if appended else 'header v2 dtb fields',
        'image_bytes': len(data),
    }
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('image', type=Path, help='boot image produced by ./unixnu bootimg')
    parser.add_argument('--fd-base', type=lambda v: int(v, 0), help='FD load base from Resources/Configs/<device>.toml [uefi_fd] base')
    parser.add_argument('--fd-size', type=lambda v: int(v, 0), help='FD size from Resources/Configs/<device>.toml [uefi_fd] size')
    parser.add_argument('--expect-header-version', type=int, help='required Android header version')
    parser.add_argument('--expect-page-size', type=int, help='required Android page size')
    parser.add_argument('--json', type=Path, help='write the full result as JSON')
    parser.add_argument('--quiet', action='store_true', help='only print failures and the summary line')
    args = parser.parse_args(argv)
    try:
        report = verify(args.image, args.fd_base, args.fd_size,
                        args.expect_header_version, args.expect_page_size)
    except OSError as error:
        print(f'verify-bootimg: cannot read {args.image}: {error}', file=sys.stderr)
        return 2

    if not args.quiet:
        for name, ok, detail in report.checks:
            mark = 'PASS' if ok else 'FAIL'
            print(f'[{mark}] {name}' + (f': {detail}' if detail else ''))
        print()
        for key, value in report.info.items():
            if isinstance(value, dict):
                print(f'{key}:')
                for sub, sub_value in value.items():
                    print(f'  {sub}: {sub_value}')
            else:
                print(f'{key}: {value}')
        print()
    failed = report.failed
    if failed:
        print(f'verify-bootimg: FAILED ({len(failed)} check(s))', file=sys.stderr)
        for name, _, detail in failed:
            print(f'  - {name}: {detail}', file=sys.stderr)
    else:
        print(f'verify-bootimg: OK ({len(report.checks)} checks) {args.image}')
    if args.json:
        payload = {'image': str(args.image), 'ok': not failed, 'info': report.info,
                   'checks': [{'name': name, 'ok': ok, 'detail': detail}
                              for name, ok, detail in report.checks]}
        args.json.write_text(json.dumps(payload, indent=2) + '\n')
        print(f'verify-bootimg: wrote {args.json}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
