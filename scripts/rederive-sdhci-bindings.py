#!/usr/bin/env python3
"""Re-derive the IOStorageFamily binding table from a delivered kernel.

The first table was derived from a prelinked kernel built without this driver.
Because LD_KERNEL_ARCHIVES participates in the kernel link, adding the driver
moves a handful of kernel symbols, which makes the kernel-bound dispatch slots
stale.  A kernel that already contains the driver has the correct addresses for
its own layout, so its base vtable is the authoritative table; rebuilding with
the corrected table (driver code unchanged) keeps the layout identical.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = Path(sys.argv[3]) if len(sys.argv) > 3 else ROOT / "kits" / "XnuPrelink"
sys.path.insert(0, str(TOOLS))
import native_prelink as np  # noqa: E402

KERNEL = Path(sys.argv[1])
OUTPUT = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "drivers" / "SdhciBlock" / "storage-bindings.json"
KERNELCACHE_SHA = "f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7"
SLIDE = -0x1000000
DATA_CONST_ORIGINAL = (0xfffffff006cb3200, 0x5C10)
TEXT_EXEC_ORIGINAL = (0xfffffff00596c000, 0x14358)
TEXT_ORIGINAL = (0xfffffff005283580, 0x1FD0)
DATA_ORIGINAL = (0xfffffff007add000, 0x3B1)
SLOTS = 190
BRK = struct.pack("<I", 0xD4200020)


def main():
    data = KERNEL.read_bytes()
    image = np.macho(data)
    segments = image["segments"]

    def read(address, length):
        if address == 0:
            return b""
        return data[np.backed_offset(segments, address, length, len(data)):][:length]

    def slid(value):
        return value + SLIDE

    data_const = (slid(DATA_CONST_ORIGINAL[0]), DATA_CONST_ORIGINAL[1])
    text_exec = (slid(TEXT_EXEC_ORIGINAL[0]), TEXT_EXEC_ORIGINAL[1])
    text = (slid(TEXT_ORIGINAL[0]), TEXT_ORIGINAL[1])
    data_segment = (slid(DATA_ORIGINAL[0]), DATA_ORIGINAL[1])

    blob = read(data_const[0], data_const[1])
    candidates = []
    for offset in range(0, len(blob) - 16 - SLOTS * 8, 8):
        if blob[offset:offset + 16] != bytes(16):
            continue
        pointers = struct.unpack_from("<" + "Q" * SLOTS, blob, offset + 16)
        if not all(any(s["va"] <= p < s["va"] + s["size"] for s in segments if s["size"])
                   for p in pointers):
            continue
        try:
            if read(pointers[0], 4) != BRK or read(pointers[1], 4) != BRK:
                continue
        except ValueError:
            continue
        a, add, ret = struct.unpack("<3I", read(pointers[7], 12))
        if (a & 0x9F00001F, add & 0xFFC003FF, ret) != (0x90000000, 0x91000000, 0xD65F03C0):
            continue
        candidates.append((data_const[0] + offset, pointers))
    if len(candidates) != 1:
        raise SystemExit(f"expected exactly one dispatch table, found {len(candidates)}")
    vtable, pointers = candidates[0]

    a, add, ret = struct.unpack("<3I", read(pointers[7], 12))
    immediate = ((a >> 5 & 0x7FFFF) << 2) | (a >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    metaclass = (pointers[7] & ~4095) + immediate * 4096 + (add >> 10 & 4095)

    constructor = None
    for start, size in (text, text_exec):
        body = read(start, size)
        for offset in range(0, len(body) - 32, 4):
            words = struct.unpack_from("<8I", body, offset)
            if (words[0], words[1]) != (0xA9BF7BFD, 0x910003FD):
                continue
            if words[2] & 0xFC000000 != 0x94000000:
                continue
            if words[3] & 0x9F00001F != 0x90000008 or words[4] & 0xFFC003FF != 0x91000108:
                continue
            if words[5:] != (0xF9000008, 0xA8C17BFD, 0xD65F03C0):
                continue
            address = start + offset
            immediate = ((words[3] >> 5 & 0x7FFFF) << 2) | (words[3] >> 29 & 3)
            if immediate & 0x100000:
                immediate -= 0x200000
            stored = ((address + 12) & ~4095) + immediate * 4096 + (words[4] >> 10 & 4095)
            if stored == vtable + 16:
                constructor = address
    if constructor is None:
        raise SystemExit("protected constructor not found")
    destructor = pointers[0] - 4
    word = struct.unpack("<I", read(destructor, 4))[0]
    if word & 0xFC000000 != 0x14000000:
        raise SystemExit("base destructor does not branch")

    kext_ranges = [(text[0], text[0] + text[1]), (text_exec[0], text_exec[0] + text_exec[1]),
                   (data_const[0], data_const[0] + data_const[1]),
                   (data_segment[0], data_segment[0] + data_segment[1])]
    kernel_slots = [index for index, pointer in enumerate(pointers)
                    if not any(start <= pointer < end for start, end in kext_ranges)]

    previous = json.loads(OUTPUT.read_text()) if OUTPUT.is_file() else {}
    result = dict(previous)
    result.update({
        "kernelcache_sha256": KERNELCACHE_SHA,
        "prelinked_kernel_sha256": hashlib.sha256(data).hexdigest(),
        "prelink_slide": hex(SLIDE),
        "provenance": "re-derived from a kernel that already contains this driver, so the "
                      "kernel-bound dispatch slots are correct for this driver's link layout "
                      "(the first table came from a prelink without the driver); pinned by the "
                      "kernelcache sha256",
        "vtable": f"0x{vtable:x}",
        "metaclass": f"0x{metaclass:x}",
        "constructor": f"0x{constructor:x}",
        "destructor": f"0x{destructor:x}",
        "kernel_bound_slots": kernel_slots,
        "slots": [f"0x{pointer:x}" for pointer in pointers],
    })
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n")
    print(f"wrote {OUTPUT}")
    print(json.dumps({key: value for key, value in result.items()
                      if key not in ("slots", "kernel_bound_slots")}, indent=2))
    print(f"kernel-bound slots: {len(kernel_slots)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
