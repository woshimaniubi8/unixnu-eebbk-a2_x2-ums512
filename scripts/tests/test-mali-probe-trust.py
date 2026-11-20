#!/usr/bin/env python3
"""Check exact diagnostic replacement and preservation of unrelated trust data."""
import importlib.util
from pathlib import Path
import struct
import uuid

source = Path(__file__).resolve().parents[1] / "replace-mali-probe-trust.py"
spec = importlib.util.spec_from_file_location("mali_trust_replace", source)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
old = dict(cdhash="11" * 20)
new = dict(cdhash="22" * 20, codesign_verified=True, entitlements=m.probe.PROBE_ENTITLEMENTS)
protected = [struct.pack("<I16sI", 2, uuid.UUID(int=i).bytes, 1) + bytes([i]) * 20 + bytes([2, 0, 0, 0])
             for i in (3, 4, 5)]
bundle = m.seed._pack_trust_modules([m.diagnostic_module(old["cdhash"]), *protected])
result = m.replace(bundle, old, new)
assert len(result) == len(bundle)
assert struct.unpack_from("<5I", result) == struct.unpack_from("<5I", bundle)
assert result[68:] == bundle[68:] == b"".join(protected)
assert result[20:68] == m.diagnostic_module(new["cdhash"])
for before, previous, current in [
    (bundle, old, {**new, "entitlements": {}}),
    (bundle, old, {**new, "codesign_verified": False}),
    (bundle, old, {**new, "cdhash": "bad"}),
    (bundle, old, {**new, "cdhash": old["cdhash"]}),
    (bundle, {"cdhash": "33" * 20}, new),
    (m.seed._pack_trust_modules(protected), old, new),
]:
    try:
        m.replace(before, previous, current)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid trust transition accepted")
print("PASS: final signed Mali probe entry, original module bytes and ADT extent preserved; invalid transitions rejected")
