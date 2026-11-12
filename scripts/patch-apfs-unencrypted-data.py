#!/usr/bin/env python3
"""Patch the APFS 'unencrypted data volume is not allowed' check.

The offline OwnerData seed is a plain `hdiutil create -fs APFS` volume.  Setting
its role to Data makes APFS run the Data-volume encryption policy, which panics
for this bring-up seed.  This patcher locates the exact role/flag check in the
prelinked APFS kext:

    ldrh w9, [x8, #0x3c4]     ; apfs_role
    cmp  w9, #0x40            ; APFS_VOL_ROLE_DATA
    b.ne <return>
    ldrb w8, [x8, #0x108]     ; apfs_fs_flags low byte
    tbnz w8, #0, <panic>      ; APFS_FS_UNENCRYPTED

and NOPs the final TBNZ.  It is a bring-up-only patch: the only Data volume in
this environment is the offline seed, and its encryption/keybag metadata is not
available.
"""
import struct
import sys
from pathlib import Path

NOP = 0xD503201F


def sections(data):
    magic, cpu, subtype, kind, ncmds, size = struct.unpack_from("<6I", data, 0)
    if (magic, cpu, subtype) != (0xFEEDFACF, 0x100000C, 0):
        raise SystemExit("not an ordinary arm64 Mach-O")
    off = 32
    result = []
    for _ in range(ncmds):
        cmd, length = struct.unpack_from("<II", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            sect = off + 72
            for _ in range(nsects):
                name = data[sect:sect + 16].rstrip(b"\0").decode()
                addr, size, offset = struct.unpack_from("<QQI", data, sect + 32)
                flags = struct.unpack_from("<I", data, sect + 64)[0]
                result.append(dict(name=name, addr=addr, size=size, offset=offset, flags=flags))
                sect += 80
        off += length
    return result


def main():
    kernel = Path(sys.argv[1])
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    data = bytearray(kernel.read_bytes())
    sects = sections(data)

    # Raw instruction pattern:
    #   ldrh w9, [x8, #0x3c4]  09 89 47 79
    #   cmp  w9, #0x40         3f 01 01 71
    #   b.ne +0xc              61 00 00 54
    #   ldrb w8, [x8, #0x108]  08 21 44 39
    #   tbnz w8, #0, <panic>   0x37...... (Rt=8)
    pattern = bytes.fromhex("098947793f0101716100005408214439")
    candidates = []
    start = 0
    while True:
        at = data.find(pattern, start)
        if at < 0:
            break
        start = at + 1
        word = struct.unpack_from("<I", data, at + 16)[0]
        if (word & 0x7F000000) != 0x37000000 or (word & 0x1F) != 8:
            continue
        for s in sects:
            if s["offset"] <= at < s["offset"] + s["size"]:
                candidates.append((s, s["addr"] + (at - s["offset"]) + 16))
                break
    if len(candidates) != 1:
        raise SystemExit(f"expected one APFS Data encryption check, found {len(candidates)}: "
                         f"{[(hex(a), hex(b)) for a, b in candidates]}")
    section, address = candidates[0]
    offset = section["offset"] + (address - section["addr"])
    old = struct.unpack_from("<I", data, offset)[0]
    struct.pack_into("<I", data, offset, NOP)
    print(f"APFS Data encryption check: {hex(address)} (file {hex(offset)}) "
          f"{old:#010x} -> NOP")
    if output:
        output.write_bytes(data)
        print(f"wrote {output}")
    else:
        kernel.write_bytes(data)
        print(f"patched {kernel}")


if __name__ == "__main__":
    main()
