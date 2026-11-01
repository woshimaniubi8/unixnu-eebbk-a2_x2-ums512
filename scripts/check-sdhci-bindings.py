#!/usr/bin/env python3
"""Fail the build when the SdhciBlock storage binding table is stale.

LD_KERNEL_ARCHIVES participates in the XNU kernel link, so adding or changing
the driver moves kernel symbols and the kernel-bound dispatch slots in
drivers/SdhciBlock/storage-bindings.json no longer point at the right functions.
This compares the prelinked kernel's IOBlockStorageDevice base vtable with the
committed table and exits non-zero when they differ, so the table has to be
re-derived from a kernel that contains the current driver
(build/rerive-sdhciblock-bindings.py) before the artifact is published.

Usage: check-sdhci-bindings.py <prelinked kernel.macho> <storage-bindings.json>
"""
import json
import struct
import sys
from pathlib import Path

# IOStorageFamily __DATA_CONST extent + the dispatch table offset inside it; both
# are pinned by the kernelcache sha256 recorded in the binding table.
VTABLE = 0xFFFFFFF005CB44C0
SLOTS = 190


def segments(data):
    magic, cpu, subtype, kind, count, size = struct.unpack_from("<6I", data)
    if (magic, cpu, subtype) != (0xFEEDFACF, 0x100000C, 0):
        raise SystemExit("not an ordinary arm64 Mach-O")
    offset, result = 32, []
    for _ in range(count):
        command, length = struct.unpack_from("<II", data, offset)
        if command == 0x19:
            name, va, vsize, fileoff, filesize = struct.unpack_from("<16s4Q", data, offset + 8)
            result.append((name.rstrip(b"\0").decode(), va, vsize, fileoff, filesize))
        offset += length
    return result


def read(data, table, address, length):
    for name, va, vsize, fileoff, filesize in table:
        if va <= address and address + length <= va + filesize:
            at = fileoff + address - va
            return data[at:at + length]
    raise SystemExit(f"address {address:#x}+{length:#x} is not file-backed")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    kernel = Path(sys.argv[1])
    bindings_path = Path(sys.argv[2])
    data = kernel.read_bytes()
    bindings = json.loads(bindings_path.read_text())
    if len(bindings["slots"]) != SLOTS:
        raise SystemExit(f"binding table has {len(bindings['slots'])} slots, expected {SLOTS}")
    wanted = [int(value, 16) for value in bindings["slots"]]
    table = segments(data)
    actual = struct.unpack_from("<" + "Q" * SLOTS, read(data, table, VTABLE + 16, SLOTS * 8))
    differences = [(index, want, got) for index, (want, got) in enumerate(zip(wanted, actual))
                   if want != got]
    if differences:
        for index, want, got in differences[:20]:
            print(f"slot {index}: table {want:#x} != kernel {got:#x}", file=sys.stderr)
        raise SystemExit(
            f"the IOStorageFamily binding table is stale ({len(differences)}/{SLOTS} slots); "
            "re-derive drivers/SdhciBlock/storage-bindings.json from a kernel that contains "
            "the current driver (build/rerive-sdhciblock-bindings.py)")
    print(f"PASS: the binding table matches the kernel's dispatch table ({SLOTS}/{SLOTS} slots)")


if __name__ == "__main__":
    sys.exit(main())
