#!/usr/bin/env python3
"""Replace only the existing owner Mali probe module after final codesigning."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import uuid

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("owner_seed_trust", ROOT / "prepare-data-seed.py")
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)
spec = importlib.util.spec_from_file_location("mali_probe_contract", ROOT / "prepare-mali-probe-boot.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def diagnostic_module(cdhash):
    if not isinstance(cdhash, str) or not re.fullmatch(r"[0-9a-f]{40}", cdhash):
        raise ValueError("invalid final CodeDirectory hash")
    identity = uuid.uuid5(uuid.NAMESPACE_URL, "org.unixnu.bbk.mali-probe:" + cdhash).bytes
    return struct.pack("<I16sI", 2, identity, 1) + bytes.fromhex(cdhash) + bytes([2, 0, 5, 0])


def replace(bundle, previous, current):
    seed._validate_trust_bundle(bundle)
    if struct.unpack_from("<I", bundle)[0] != 4:
        raise ValueError("expected the qualified four-module bundle")
    if current.get("codesign_verified") is not True or current.get("entitlements") != probe.PROBE_ENTITLEMENTS:
        raise ValueError("new probe signature and exact own-client entitlements were not verified")
    offsets = [*struct.unpack_from("<4I", bundle, 4), len(bundle)]
    modules = [bundle[a:b] for a, b in zip(offsets, offsets[1:])]
    if modules[0] != diagnostic_module(previous["cdhash"]):
        raise ValueError("first module does not identify the previous deployed Mali probe")
    if previous["cdhash"] == current["cdhash"]:
        raise ValueError("new probe has the old CodeDirectory hash")
    result = seed._pack_trust_modules([diagnostic_module(current["cdhash"]), *modules[1:]])
    if len(result) != len(bundle) or result[:offsets[0]] != bundle[:offsets[0]] or result[offsets[1]:] != bundle[offsets[1]:]:
        raise ValueError("unrelated module or bundle layout changed")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("previous_report", type=Path)
    parser.add_argument("current_report", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    previous = json.loads(args.previous_report.read_text())
    current = json.loads(args.current_report.read_text())
    before = args.bundle.read_bytes()
    after = replace(before, previous, current)
    with args.output.open("xb") as f:
        f.write(after)
    print(json.dumps(dict(bytes=len(after), before_sha256=hashlib.sha256(before).hexdigest(),
                         after_sha256=hashlib.sha256(after).hexdigest(), previous_cdhash=previous["cdhash"],
                         current_cdhash=current["cdhash"], preserved_modules=3, adt_extent_unchanged=True), indent=2))


if __name__ == "__main__":
    main()
