#!/usr/bin/env python3
"""Build the board service, pinned Mali objects and exact board WCN image."""
import hashlib,json,subprocess,sys
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
firmware=SOURCE/'firmware/wcnmodem-1h190-sc2355.bin'
raw=firmware.read_bytes()
assert len(raw)==0xe7400 and hashlib.sha256(raw).hexdigest()=='59e177b4cb195c63a0021164d286c13c5c4fc9be9d430457451db40dc8caf554'
asm=OUT/'SprdWcnFirmware.S';obj=OUT/'SprdWcnFirmware.o'
asm.write_text('.section __TEXT,__const\n.p2align 4\n.globl _sprd_wcn_firmware_start\n_sprd_wcn_firmware_start:\n.incbin "'+firmware.resolve().as_posix()+'"\n.globl _sprd_wcn_firmware_end\n_sprd_wcn_firmware_end:\n')
clang=record['arguments'][0]
command=[clang,'-arch','arm64','-target','arm64-apple-ios17.0','-c',str(asm),'-o',str(obj)]
(OUT/'SprdWcnFirmware-command.json').write_text(json.dumps(command,indent=2)+'\n')
r=subprocess.run(command,cwd=record['directory'],capture_output=True,text=True)
(OUT/'SprdWcnFirmware.log').write_text(r.stdout+r.stderr)
if r.returncode:raise SystemExit((r.stdout+r.stderr)[-7000:])
objects.append(str(obj))
subprocess.run(['xcrun','ar','r',str(OUT/'libSprdPlatform.a'),*objects],check=True)
print('PASS: SprdPlatform + pinned Mali VM/MMU/job objects + factory SC2355 WCN image')
