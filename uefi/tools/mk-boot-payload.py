"""Assemble the BBK A2 boot payload: boot shim followed by the firmware volume.

The shim is linked at 0x80080000, which is where Unisoc cboot drops the boot
partition kernel field, and it copies the firmware volume to FD_BASE
(0x80800000) before branching into it.  The two blobs are simply concatenated,
exactly like Project Silicium's build_uefi.py does for other devices.

The AArch64 Image header at the start of the shim already carries the exact
total image size at offset 16 (the assembler knows FD_SIZE and the shim length,
see BootShim.S).  This script only checks it; it does not patch it, so the
value UniXNU's `compile_shim()` validates and the value in the packed payload
are identical.  The output file is zero-padded to a 4 KiB boundary for flashing
convenience, but the header still describes shim + FD exactly.

Usage: mk-boot-payload.py <shim.bin> <firmware.fd> <output.bin>

Optional environment:
  BBK2_RAMDISK=<file>  append "BBK2" + u64 size + a bare FAT image after the
                       firmware volume.  TODO: BootShim does not rescue this
                       blob yet; when it does it should stage it at 0x95000000
                       (see Platforms/Unisoc/bbkA2Pkg/Drivers/BbkA2PayloadDxe).
"""

import os
import struct
import sys

IMAGE_SIZE_OFFSET = 16
ARM64_MAGIC_OFFSET = 56
ARM64_MAGIC = b"ARM\x64"


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2

    shim_path, fd_path, out_path = argv[1], argv[2], argv[3]

    shim = bytearray(open(shim_path, "rb").read())
    fd = open(fd_path, "rb").read()

    if len(shim) < ARM64_MAGIC_OFFSET + 8:
        print("shim is too small to hold an Image header: %d bytes" % len(shim))
        return 1

    if shim[ARM64_MAGIC_OFFSET:ARM64_MAGIC_OFFSET + 4] != ARM64_MAGIC:
        print("shim has no ARMd magic at offset %d" % ARM64_MAGIC_OFFSET)
        return 1

    text_offset, image_size = struct.unpack_from("<QQ", shim, 8)
    want = len(shim) + len(fd)
    if image_size != want:
        print(
            "shim image_size 0x%x != shim + FD (0x%x); rebuild the shim with "
            "-DFD_SIZE=0x%x" % (image_size, want, len(fd))
        )
        return 1

    out = bytearray(shim)
    out += fd
    while len(out) % 4096:
        out += b"\0"

    rd_path = os.environ.get("BBK2_RAMDISK", "")
    if rd_path and os.path.isfile(rd_path):
        rd = open(rd_path, "rb").read()
        out += b"BBK2"
        out += struct.pack("<Q", len(rd))
        out += rd
        print("ramdisk %s: %d bytes appended at payload offset %d"
              % (rd_path, len(rd), len(out) - len(rd) - 12))

    with open(out_path, "wb") as f:
        f.write(out)

    print(
        "shim %d bytes, firmware 0x%x bytes, payload 0x%x bytes, "
        "text_offset 0x%x, image_size 0x%x"
        % (len(shim), len(fd), len(out), text_offset, image_size)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
