#!/usr/bin/env python3
"""Build the board service plus pinned, board-neutral Mali VM/job objects."""
import json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
subprocess.run([sys.executable,ROOT/'scripts/build-arm-driver.py','SprdPlatform'],check=True)
OUT=ROOT/'build/sprdplatform';SOURCE=ROOT/'drivers/SprdPlatform'
record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
names=['NativeMaliGpu','NativeMaliGpuCore','NativeMaliVm','NativeMaliMmu','NativeMaliFault','NativeMaliJob','NativeMaliJobSlot']
sources=[SOURCE/'SprdMaliTest.cpp']+[SOURCE/'mali'/(name+'.cpp') for name in names]
objects=[]
for source in sources:
    obj=OUT/(source.stem+'.o');args=record['arguments'];command=[];i=0
    while i<len(args):
        value=args[i]
        if value==record['file']:command.append(str(source))
        elif value=='-o':command+=['-o',str(obj)];i+=1
        elif value=='-Xclang' and args[i+1].startswith('-W'):i+=1
        elif not value.startswith('-W'):command.append(value)
        i+=1
    command+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion','-Wno-unnecessary-virtual-specifier','-DNATIVE_MALI_M3_WRITES=1','-DNATIVE_MALI_M4_WRITES=1','-DNATIVE_MALI_M6_WRITES=1','-I',str(ROOT/'.cache/xnu-work/osfmk'),'-I',str(SOURCE/'mali')]
    (OUT/(source.stem+'-command.json')).write_text(json.dumps(command,indent=2)+'\n')
    r=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True)
    (OUT/(source.stem+'.log')).write_text(r.stdout+r.stderr)
    if r.returncode:raise SystemExit((r.stdout+r.stderr)[-7000:])
    objects.append(str(obj))
subprocess.run(['xcrun','ar','r',str(OUT/'libSprdPlatform.a'),*objects],check=True)
print('PASS: SprdPlatform + pinned Mali VM/MMU/owned-memory job objects')
