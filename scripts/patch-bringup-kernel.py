#!/usr/bin/env python3
"""Apply the BBK A2/X2 bring-up patches to the prelinked XNU kernel.

The kernel is built from apple-oss-distributions/xnu with the BCM2837 machine
configuration (the only non-Apple machine configuration XNU carries) plus the
UMS512 patch series.  Two pieces of that machine configuration assume Apple
hardware and abort the boot on UMS512:

1. ``pexpert/arm/pe_init.c`` maps the BCM2837/Raspberry-Pi VRAM panic log at
   physical 0x3C0FC000 and dereferences it.  Nothing is mapped there on this
   SoC, so the very first ``PE_create_console()`` takes a data abort.  Retarget
   the mapping to the board's display reservation (0x9E000000, real DRAM).

2. The prelinked AppleMobileFileIntegrity kext panics with
   ``"AMFI: No PMGR?" @ConfigurationSettings.cpp:235`` because the UMS512 has
   no Apple PMGR service.  The panic helper is reached from a PMGR pointer
   NULL check; rewrite the helper so it restores its caller's frame and
   returns instead of panicking.

Both patches are pattern based so they survive unrelated rebuilds, and the
script is idempotent.

Usage: patch-bringup-kernel.py <kernel.macho>
"""

import struct
import sys

VRAM_OLD = struct.pack("<II", 0x52980000, 0x72A781E0)  # mov w0,#0xC000 ; movk w0,#0x3C0F,lsl#16
VRAM_NEW = struct.pack("<II", 0x52800000, 0x72B3C000)  # mov w0,#0 ; movk w0,#0x9E00,lsl#16

AMFI_FMT = b'"AMFI: No PMGR?\\n" @%s:%d'
AMFI_HELPER_EPILOGUE = struct.pack(
    "<IIII",
    0xA9417BFD,  # ldp x29, x30, [sp, #16]
    0x910083FF,  # add sp, sp, #0x20
    0xD65F03C0,  # ret
    0xD503201F,  # nop
)


def read_segments(data):
    ncmds = struct.unpack_from("<I", data, 16)[0]
    offset = 32
    segments = []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, offset)
        if cmd == 0x19:  # LC_SEGMENT_64
            name = data[offset + 8:offset + 24].split(b"\x00")[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, offset + 24)
            segments.append((name, vmaddr, vmsize, fileoff, filesize))
        offset += cmdsize
    return segments


def file_to_va(segments, offset):
    for _name, vmaddr, _vmsize, fileoff, filesize in segments:
        if fileoff <= offset < fileoff + filesize:
            return vmaddr + (offset - fileoff)
    return None


def va_to_file(segments, address):
    for _name, vmaddr, _vmsize, fileoff, filesize in segments:
        if vmaddr <= address < vmaddr + filesize:
            return fileoff + (address - vmaddr)
    return None


def sign_extend(value, bits):
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return value


def find_string_ref(segments, data, target):
    """Return the file offset of the ADRP+ADD pair that materialises target."""
    for _name, vmaddr, _vmsize, fileoff, filesize in segments:
        if not filesize:
            continue
        blob = data[fileoff:fileoff + filesize]
        for i in range(0, len(blob) - 8, 4):
            insn = struct.unpack_from("<I", blob, i)[0]
            if (insn & 0x9F000000) != 0x90000000:  # ADRP
                continue
            rd = insn & 0x1F
            immlo = (insn >> 29) & 3
            immhi = (insn >> 5) & 0x7FFFF
            imm = sign_extend((immhi << 2) | immlo, 21)
            page = ((vmaddr + i) & ~0xFFF) + (imm << 12)
            insn2 = struct.unpack_from("<I", blob, i + 4)[0]
            if (insn2 & 0xFFC00000) != 0x91000000 or (insn2 & 0x1F) != rd:  # ADD
                continue
            imm12 = (insn2 >> 10) & 0xFFF
            shift = 12 if ((insn2 >> 22) & 3) == 1 else 0
            if page + (imm12 << shift) == target:
                return fileoff + i
    return None


def patch_vram_panic_log(data, segments):
    hits = []
    offset = data.find(VRAM_OLD)
    while offset >= 0:
        hits.append(offset)
        offset = data.find(VRAM_OLD, offset + 1)
    if not hits:
        if data.find(VRAM_NEW) >= 0:
            print("vram panic log: already patched")
            return True
        print("vram panic log: pattern not found", file=sys.stderr)
        return False
    if len(hits) != 1:
        print("vram panic log: expected one occurrence, found %d" % len(hits), file=sys.stderr)
        return False
    data[hits[0]:hits[0] + 8] = VRAM_NEW
    print("vram panic log: 0x3C0FC000 -> 0x9E000000 at file offset 0x%x" % hits[0])
    return True


def patch_amfi_no_pmgr(data, segments):
    fmt_offset = data.find(AMFI_FMT)
    if fmt_offset < 0:
        print("amfi no-pmgr: panic string not found", file=sys.stderr)
        return False
    fmt_va = file_to_va(segments, fmt_offset)
    if fmt_va is None:
        print("amfi no-pmgr: panic string outside any segment", file=sys.stderr)
        return False
    ref_offset = find_string_ref(segments, data, fmt_va)
    if ref_offset is None:
        print("amfi no-pmgr: no reference to the panic string", file=sys.stderr)
        return False
    # Layout of the helper:
    #   +0x00 adrp x8, <file string>
    #   +0x04 add  x8, x8, #line-of-file-string
    #   +0x08 mov  w9, #235
    #   +0x0c stp  x8, x9, [sp]
    #   +0x10 adrp x0, <panic string>   <- ref_offset
    #   +0x14 add  x0, x0, ...
    #   +0x18 bl   panic
    helper = ref_offset - 0x10
    if helper < 0:
        print("amfi no-pmgr: helper offset underflow", file=sys.stderr)
        return False
    current = bytes(data[helper:helper + 16])
    if current == AMFI_HELPER_EPILOGUE:
        print("amfi no-pmgr: already patched")
        return True
    insn = struct.unpack_from("<I", data, helper)[0]
    call = struct.unpack_from("<I", data, helper + 0x18)[0]
    if (insn & 0x9F000000) != 0x90000000 or (call & 0xFC000000) != 0x94000000:
        print("amfi no-pmgr: unexpected helper layout (0x%08x/0x%08x)" % (insn, call), file=sys.stderr)
        return False
    data[helper:helper + 16] = AMFI_HELPER_EPILOGUE
    print("amfi no-pmgr: panic helper replaced with epilogue at file offset 0x%x" % helper)
    return True


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    path = sys.argv[1]
    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    segments = read_segments(data)
    if not segments:
        print("no LC_SEGMENT_64 commands found", file=sys.stderr)
        return 1
    ok = patch_vram_panic_log(data, segments)
    ok = patch_amfi_no_pmgr(data, segments) and ok
    if not ok:
        return 1
    with open(path, "wb") as handle:
        handle.write(data)
    print("bring-up kernel patches applied to %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
