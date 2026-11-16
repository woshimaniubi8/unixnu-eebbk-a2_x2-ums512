#!/usr/bin/env python3
"""Install the board's exact DarwinBoot handoff and ADT ABI reservation."""
import argparse,shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('core',type=Path);a=p.parse_args()
device=Path(__file__).resolve().parents[1]
target=a.core/'drivers/DarwinBoot/DarwinBootPkg/Driver'
assert (target/'DarwinBoot.c').is_file()
for name in ['DarwinBoot.c','DarwinMemory.c.inc','BbkMemoryMap.h']:
    source=device/'memory'/name;dest=target/name
    stage=dest.with_name(dest.name+'.memory-stage');shutil.copy2(source,stage);stage.replace(dest)
inf=target/'DarwinBoot.inf';s=inf.read_text()
if '  FdtLib\n' not in s:
    assert s.count('  BaseLib\n')==1
    s=s.replace('  BaseFdtLib\n','')
    s=s.replace('  BaseLib\n','  BaseLib\n  FdtLib\n',1);inf.write_text(s,newline='\n')
tree=a.core/'scripts/unixnu/devicetree.py';s=tree.read_text()
anchor="        chosen.append(('darwinboot-memory-size', struct.pack('<Q', size)))"
if "'unixnu,physical-memory-map'" not in s:
    assert s.count(anchor)==1
    s=s.replace(anchor,anchor+"\n    if payload.get('physical_memory_map', False):\n        chosen.append(('unixnu,physical-memory-map', bytes(576)))",1)
    tree.write_text(s,newline='\n')
print('Installed version1 physical-memory ownership ABI and exact board loader')
