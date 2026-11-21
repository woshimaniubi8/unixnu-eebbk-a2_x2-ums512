#!/usr/bin/env python3
"""Verify the final kernel still carries the audited original accelerator base."""
import hashlib
import json
import struct
import sys
from pathlib import Path

args = sys.argv[1:]
reference_only = "--reference-only" in args
args = [value for value in args if value != "--reference-only"]
if len(args) != 2:
    raise SystemExit("usage: check-mali-system-accelerator.py KERNEL XNU_PRELINK_TOOLS [--reference-only]")
kernel = Path(args[0])
tools = Path(args[1])
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(tools))
from native_prelink import backed_offset, exports, macho, unserialize_xml

reference = json.loads((root / "drivers/SprdPlatform/mali/OriginalAcceleratorABI.json").read_text())
data = kernel.read_bytes()
image = macho(data)
segments = image["segments"]


def read(address, length):
    offset = backed_offset(segments, address, length, len(data))
    return data[offset:offset + length]


info = next(segment for segment in segments if segment["name"] == "__PRELINK_INFO")
metadata = unserialize_xml(read(info["va"], info["filesize"]))
entry = next(item for item in metadata["_PrelinkInfoDictionary"]
             if item["CFBundleIdentifier"] == "com.apple.iokit.IOGPUFamily")
header = backed_offset(segments, entry["_PrelinkExecutableLoadAddr"], 32, len(data))
module = macho(data, header)
module_text = next(segment for segment in module["segments"] if segment["name"] == "__TEXT_EXEC")
assert hashlib.sha256(read(module_text["va"], module_text["filesize"])).hexdigest() == \
    reference["original_iogpu_text_sha256"], "Original IOGPUFamily executable changed"

symbol_names = {name: address for address, names in exports(data, image).items() for name in names}
service_vtable = struct.unpack("<168Q", read(symbol_names["__ZTV9IOService"] + 16, 168 * 8))
candidates = []
for segment in module["segments"]:
    if segment["name"] in ("__TEXT_EXEC", "__LINKEDIT") or not segment["filesize"]:
        continue
    body = read(segment["va"], segment["filesize"])
    # Match the same stable IOService virtual entry used by the original
    # read-only audit. The getMetaClass slot is not necessarily materialized
    # as a directly searchable pointer in every prelinked segment.
    needle = struct.pack("<Q", service_vtable[2])
    at = 0
    while True:
        index = body.find(needle, at)
        if index < 0:
            break
        at = index + 1
        vptr = segment["va"] + index - 16
        if vptr & 7:
            continue
        try:
            table = struct.unpack("<168Q", read(vptr, 168 * 8))
        except (ValueError, struct.error):
            continue
        if [i for i, pair in enumerate(zip(table, service_vtable)) if pair[0] != pair[1]] != [0, 1, 7]:
            continue
        try:
            a, b, ret = struct.unpack("<3I", read(table[7], 12))
        except (ValueError, struct.error):
            continue
        if a & 0x9F00001F != 0x90000000 or b & 0xFFC003FF != 0x91000000 or ret != 0xD65F03C0:
            continue
        candidates.append((vptr, table))

assert len(candidates) == 1, f"Expected one original IOAcceleratorES vtable, found {len(candidates)}"
vptr, table = candidates[0]
assert vptr == int(reference["vptr"], 16), "Original accelerator vtable moved or changed"
get_meta = int(reference["get_meta_class"], 16)
assert table[7] == get_meta, "Original accelerator getMetaClass slot changed"
assert struct.unpack("<I", read(table[0], 4))[0] & 0xFC000000 == 0x14000000, \
    "Original non-deleting destructor entry changed"

for address, digest in reference["original_entry_sha256"].items():
    assert hashlib.sha256(read(int(address, 16), 32)).hexdigest() == digest, \
        f"Original accelerator ABI entry changed at {address}"

if not reference_only:
    for value in (b"IOAcceleratorES", b"NativeSprdMaliService", b"MetalPluginName",
                  b"MetalPluginClassName", b"VivaMTLDriver", b"VMDevice",
                  b"NativeSandboxGrantProcess", b"backboardd,SpringBoard"):
        assert value in data, f"System accelerator publication marker absent: {value!r}"

print("PASS: exact original IOAcceleratorES 136-byte/168-slot ABI and four entry checks")
print("PASS: original IOGPUFamily text unchanged")
if not reference_only:
    print("PASS: system Metal and scoped grant markers linked")
