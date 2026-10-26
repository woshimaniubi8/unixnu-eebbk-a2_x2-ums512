#!/usr/bin/env python3
"""Pack a boot image whose layout matches the stock eebbk A2 / T610YP boot.bin.

The stock boot image of this board is ANDROID! header v2 with:
  page size 2048, kernel_addr 0x8000, ramdisk_addr 0x5400000, tags_addr 0x100,
  a RAW (not gzip) kernel image in the kernel field, and the device tree in the
  header dtb_size/dtb_addr fields (dtb_size=157748, dtb_addr=0x1f00000).

`./unixnu bootimg` instead appends the DTB after a gzip-compressed kernel
(Image.gz-dtb style) and cannot fill the header v2 dtb fields (AOSP mkbootimg
rejects an empty --dtb for v2). This script keeps UniXNU's BootShim compiler and
mkbootimg.py but produces the stock layout:

    kernel field = BootShim || UEFI firmware volume      (raw, no compression)
    dtb field    = device tree                            (header v2 dtb_size/dtb_addr)
    ramdisk      = empty

Parameters are read from the device repository's board.toml ([bootimg.image]
overrides the UEFI config [boot_image]) and from the UEFI checkout's
Resources/Configs/<firmware.config>.toml ([uefi_fd], [boot_shim], [boot_image]).

Usage:
    python3 build/pack-device-bootimg.py \
        --unixnu _repos/UniXNU \
        --device build/bbk-a2-device \
        --uefi build/uefi-bbk-a2 \
        --fd build/placeholder/SILICIUM_UEFI.fd \
        --dtb build/placeholder/bbk-a2-device-tree.dtb \
        -o dist/boot.img --report build/boot-devicelayout.json
"""
import argparse
import gzip
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

DEFAULTS = {'header_version': 0, 'base': 0, 'kernel_offset': 0x8000,
            'tags_offset': 0x1e00000, 'pagesize': 4096, 'dtb_offset': 0x1f00000}


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def merge_image_settings(device, uefi, config_name):
    """Same precedence as unixnu.bootimg.settings(): defaults < UEFI config < device board.toml."""
    board = tomllib.loads((Path(device) / 'board.toml').read_text())
    config_path = Path(uefi) / 'Resources/Configs' / f'{config_name}.toml'
    config = tomllib.loads(config_path.read_text())
    values = dict(DEFAULTS)
    values.update(config.get('boot_image', {}))
    values.update(board.get('bootimg', {}).get('image', {}))
    return values, config, board, config_path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--unixnu', default='_repos/UniXNU', help='UniXNU build tree (for bootimg.compile_shim)')
    parser.add_argument('--device', default='build/bbk-a2-device', help='device repository (board.toml)')
    parser.add_argument('--uefi', default='build/uefi-bbk-a2', help='Mu-Silicium-style checkout (BootShim + mkbootimg + config)')
    parser.add_argument('--config', default=None, help='override the UEFI device config toml')
    parser.add_argument('--fd', default='build/placeholder/SILICIUM_UEFI.fd')
    parser.add_argument('--dtb', default='build/placeholder/bbk-a2-device-tree.dtb')
    parser.add_argument('--ramdisk', default='none', help='ramdisk file, or none (default)')
    parser.add_argument('--gzip-kernel', action='store_true',
                        help='gzip the BootShim+FD kernel (reference variant only; the device expects a raw kernel)')
    parser.add_argument('-o', '--output', default='dist/boot.img')
    parser.add_argument('--report', default=None, help='write a JSON report here')
    args = parser.parse_args(argv)

    unixnu = Path(args.unixnu).resolve()
    device = Path(args.device).resolve()
    uefi = Path(args.uefi).resolve()
    fd = Path(args.fd).resolve()
    dtb = Path(args.dtb).resolve()
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    board = tomllib.loads((device / 'board.toml').read_text())
    config_name = board.get('firmware', {}).get('config')
    if not config_name:
        raise SystemExit(f'{device}/board.toml has no [firmware] config')
    image, config, board, config_path = merge_image_settings(device, uefi, config_name)

    for path, what in ((unixnu / 'scripts/unixnu/bootimg.py', 'UniXNU bootimg.py'),
                       (uefi / 'BootShim/BootShim.S', 'BootShim.S'),
                       (uefi / 'Resources/Scripts/mkbootimg.py', 'mkbootimg.py'),
                       (config_path, 'UEFI device config toml'), (fd, 'firmware volume'), (dtb, 'device tree')):
        if not path.exists():
            raise SystemExit(f'missing {what}: {path}')

    fd_cfg = config.get('uefi_fd', {})
    base, size = fd_cfg.get('base'), fd_cfg.get('size')
    if base is None or size is None:
        raise SystemExit(f'{config_path} has no [uefi_fd] base/size')
    if fd.stat().st_size != size:
        raise SystemExit(f'firmware volume is {fd.stat().st_size:#x} bytes, config says {size:#x}')
    requires_header = config.get('boot_shim', {}).get('requires_kernel_header', True)
    kernel_cfg = config.get('boot_image_kernel', {})
    compression = kernel_cfg.get('kernel_compression', 'none')
    if args.gzip_kernel:
        compression = 'gzip'
    if kernel_cfg.get('append_dtb') and not args.gzip_kernel:
        raise SystemExit('config asks for append_dtb; this packer always puts the DTB in the header v2 fields')
    if compression not in ('none', 'gzip'):
        raise SystemExit(f'unsupported [boot_image_kernel] kernel_compression = {compression!r}')

    # UniXNU's own shim compiler: identical output to `./unixnu bootimg`.
    sys.path.insert(0, str(unixnu / 'scripts'))
    from unixnu import bootimg  # noqa: E402

    with tempfile.TemporaryDirectory(prefix='pack-bootimg-') as name:
        scratch = Path(name)
        shim = bootimg.compile_shim(uefi, base, size, requires_header, scratch)
        combined = shim + fd.read_bytes()
        kernel = scratch / 'kernel'
        if compression == 'gzip':
            kernel.write_bytes(gzip.compress(combined, mtime=0))
        else:
            kernel.write_bytes(combined)
        ramdisk = scratch / 'ramdisk'
        if args.ramdisk and args.ramdisk != 'none':
            ramdisk.write_bytes(Path(args.ramdisk).read_bytes())
        else:
            ramdisk.write_bytes(b'')
        command = [sys.executable, str(uefi / 'Resources/Scripts/mkbootimg.py'),
                   '--kernel', str(kernel), '--ramdisk', str(ramdisk),
                   '--dtb', str(dtb), '-o', str(output)]
        for key in ('base', 'kernel_offset', 'ramdisk_offset', 'tags_offset', 'dtb_offset',
                    'second_offset', 'pagesize', 'header_version', 'os_version', 'cmdline'):
            if key in image:
                value = image[key]
                command += [f'--{key}', hex(value) if isinstance(value, int) else str(value)]
        subprocess.run(command, check=True)
        header = output.read_bytes()
        kernel_size = struct.unpack_from('<I', header, 8)[0]
        dtb_size = struct.unpack_from('<I', header, 1648)[0]
        dtb_addr = struct.unpack_from('<Q', header, 1652)[0]
        report = {
            'image': str(output), 'image_bytes': output.stat().st_size,
            'image_sha256': sha256_file(output),
            'layout': (f'{compression} BootShim+FD in kernel, DTB in header v2 dtb fields'),
            'kernel_bytes': kernel_size, 'kernel_sha256': sha256_file(kernel),
            'kernel_compression': compression,
            'ramdisk_bytes': ramdisk.stat().st_size,
            'dtb_bytes': dtb_size, 'dtb_addr': hex(dtb_addr), 'dtb_sha256': sha256_file(dtb),
            'fd_bytes': fd.stat().st_size, 'fd_sha256': sha256_file(fd),
            'fd_base': hex(base), 'fd_placeholder': fd.name == 'SILICIUM_UEFI.fd' and 'placeholder' in str(fd),
            'shim_bytes': len(shim), 'shim_sha256': hashlib.sha256(shim).hexdigest(),
            'header_version': image.get('header_version'), 'pagesize': image.get('pagesize'),
            'kernel_addr': hex(image.get('base', 0) + image.get('kernel_offset', 0)),
            'ramdisk_addr': hex(image.get('base', 0) + image.get('ramdisk_offset', 0)),
            'tags_addr': hex(image.get('base', 0) + image.get('tags_offset', 0)),
            'cmdline': image.get('cmdline'),
            'hardware_tested': False,
        }
    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
