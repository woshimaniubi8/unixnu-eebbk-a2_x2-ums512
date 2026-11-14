#!/usr/bin/env python3
"""Qualify the exact original IOHID implementation used by SprdThermal."""
import hashlib
import json
import struct
import sys
from pathlib import Path


def check(path, tools=None):
    sys.path.insert(0, str(tools or Path.cwd() / 'kits/XnuPrelink'))
    from native_prelink import macho, backed_offset, unserialize_xml
    d = Path(path).read_bytes()
    m = macho(d)
    def read(a, n):
        o = backed_offset(m['segments'], a, n, len(d))
        return d[o:o+n]
    def words(a, n): return struct.unpack(f'<{n}I', read(a, 4*n))
    info = next(s for s in m['segments'] if s['name'] == '__PRELINK_INFO')
    entries = unserialize_xml(read(info['va'], info['filesize']))['_PrelinkInfoDictionary']
    hid = next(e for e in entries if e['CFBundleIdentifier'] == 'com.apple.iokit.IOHIDFamily')
    assert 'IOHIDFamily-2031.120.4' in hid['NSHumanReadableCopyright']
    assert hid.get('CFBundleExecutable') == 'IOHIDFamily'
    assert hid.get('_PrelinkExecutableLoadAddr') and hid.get('_PrelinkKmodInfo')
    child = next(e for e in entries if e['CFBundleIdentifier'] == 'com.apple.iokit.IOHIDEventDriver')
    assert not child.get('CFBundleExecutable') and not child.get('_PrelinkExecutableLoadAddr')
    # Verify sizeof in the original metaclass constructors, not a guessed extent.
    assert words(0xfffffff004aeafd4, 1)[0] == 0x52801903 # mov w3,#0xc8: IOHIDEventService
    assert words(0xfffffff004af0990, 1)[0] == 0x52802103 # mov w3,#0x108: IOHIDEventDriver
    assert words(0xfffffff004afbdc8, 4) == (0xa9bc5ff8,0xa90157f6,0xa9024ff4,0xa9037bfd)
    # This factory makes a type-15 / 20-byte event and stores the actual IOFixed.
    assert words(0xfffffff004afbe34, 1)[0] == 0x528001e9
    assert words(0xfffffff004afbe4c, 1)[0] == 0xb9001113
    vtable = struct.unpack('<219Q', read(0xfffffff005d05e10, 219*8))
    assert vtable[21] == 0xfffffff004aeb0d8
    assert vtable[86] == 0xfffffff004aeb150
    assert vtable[178] == 0xfffffff004aed15c # getProduct, the per-service daemon identity
    assert vtable[198] == 0xfffffff004aee85c
    assert vtable[201] == 0xfffffff004aeeba0
    assert read(0xfffffff005d05e10+219*8,24) == bytes(24)
    assert words(vtable[168], 2) == (0x52800020,0xd65f03c0) # handleStart returns true
    return {'kernel_sha256':hashlib.sha256(d).hexdigest(), 'iohid_version':'2031.120.4',
            'slots':219, 'class_bytes':200, 'copy_event_slot':201,
            'dispatch_event_slot':198, 'factory':'0xfffffff004afbdc8', 'passed':True}


if __name__ == '__main__':
    print(json.dumps(check(sys.argv[1], Path(sys.argv[2]) if len(sys.argv)>2 else None), indent=2))
