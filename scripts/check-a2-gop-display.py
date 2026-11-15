#!/usr/bin/env python3
"""Reject a display image whose original or native dispatch differs from its ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('kernel', type=Path)
p.add_argument('--prelink-tools', type=Path, required=True)
a = p.parse_args()
sys.path.insert(0, str(a.prelink_tools.resolve()))
from native_prelink import macho, backed_offset

reference = json.loads((Path(__file__).resolve().parents[1] / 'drivers/A2GopDisplay/original-abi.json').read_text())
data = a.kernel.read_bytes()
image = macho(data)

def require(value, message):
    if not value:
        raise SystemExit(message)

def read(va, count):
    offset = backed_offset(image['segments'], va, count, len(data))
    return data[offset:offset+count]

st = next(offset for cmd, offset, _ in image['commands'] if cmd == 2)
symbols, count, strings, _ = struct.unpack_from('<4I', data, st+8)
names = {}
targets = set(reference['native_symbols']) | {
    '___cxa_pure_virtual', '__ZTV19NativeLegacyDisplay',
    '_native_legacy_display_probe', '_native_legacy_display_late_probe',
    '__GLOBAL__sub_I_NativeLegacyDisplay.cpp',
}
for i in range(count):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', data, symbols+16*i)
    if ix and not kind & 0xe0 and kind & 0xe == 0xe and section:
        name = data[strings+ix:data.index(b'\0', strings+ix)].decode()
        if name not in targets and not name.startswith(('__ZN19NativeLegacyDisplay','__ZNK19NativeLegacyDisplay')):
            continue
        require(name not in names or names[name] == va, 'Ambiguous defined symbol '+name)
        names[name] = va

original = struct.unpack('<358Q', read(int(reference['legacy_vptr'],16),358*8))
native_by_old = {}
for name, old in reference['native_symbols'].items():
    if name in names:
        native_by_old.setdefault(old,set()).add(names[name])
pure = set(reference['abstract_slots'])
expected = [int(entry['address'],16) for entry in reference['methods']]
for slot, value in enumerate(original):
    if slot in pure:
        require(value == names['___cxa_pure_virtual'], f'Original pure slot {slot}')
    elif expected[slot] in native_by_old:
        require(value in native_by_old[expected[slot]], f'Original native rebind slot {slot}')
    else:
        require(value == expected[slot], f'Original family method slot {slot}')

table = names['__ZTV19NativeLegacyDisplay']+16
actual = struct.unpack('<358Q',read(table,358*8))
own = {va for name,va in names.items() if name.startswith(('__ZN19NativeLegacyDisplay','__ZNK19NativeLegacyDisplay'))}
overrides = []
for slot, value in enumerate(actual):
    if value in own:
        overrides.append(slot)
    else:
        require(value == original[slot], f'Inherited final native vtable slot {slot}')
require(pure <= set(overrides), 'A pure hardware leaf remains unresolved')
require(set(overrides) == set([0,1,7,14,84,86,87,141,155,184,188,288,290,296,301]+list(pure)), 'Unexpected display override ABI')
require('_native_legacy_display_probe' in names and '_native_legacy_display_late_probe' in names, 'Missing early/late display lifecycle')
require('__GLOBAL__sub_I_NativeLegacyDisplay.cpp' in names, 'Missing deferred metaclass initializer')
require(b'bbk display: published original Legacy' in data, 'Missing actual publication diagnostic')

result = dict(kernel_sha256=hashlib.sha256(data).hexdigest(),original_slots_checked=358,
              concrete_slots_checked=358,owned_overrides=overrides,
              native_rebindings_checked=True,late_initialization_present=True,
              original_client_reused=True,hardware_tested=False)
a.kernel.with_suffix('.display-abi.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS: 358 original + 358 concrete display slots; exact native rebindings; deferred lifecycle')
