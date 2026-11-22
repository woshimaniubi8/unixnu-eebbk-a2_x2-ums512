#!/usr/bin/env python3
"""Build the board service, pinned Mali objects and exact board WCN image."""
import hashlib,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
subprocess.run([sys.executable,ROOT/'scripts/build-arm-driver.py','SprdPlatform'],check=True)
OUT=ROOT/'build/sprdplatform';SOURCE=ROOT/'drivers/SprdPlatform'
record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
names=['NativeMaliGpu','NativeMaliGpuCore','NativeMaliVm','NativeMaliMmu','NativeMaliFault','NativeMaliJob','NativeMaliJobSlot','NativeMaliMemory','NativeMaliBoTable','NativeMaliAbiValidate','NativeMaliUserClient','NativeMaliXnuSupport']
sources=[SOURCE/'SprdMaliTest.cpp',SOURCE/'SprdMaliService.cpp',SOURCE/'SprdWifiRuntime.cpp',SOURCE/'SprdWifiNet.cpp']+[SOURCE/'mali'/(name+'.cpp') for name in names]
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
    if source.name=='SprdWifiNet.cpp':
        llvm=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin'
        late=OUT/'SprdWifiNet.late.o'
        subprocess.run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func',
                        '--globalize-symbol=__GLOBAL__sub_I_SprdWifiNet.cpp',obj,late],check=True)
        symbols=subprocess.check_output(['xcrun','nm',late],text=True)
        assert ' T __GLOBAL__sub_I_SprdWifiNet.cpp' in symbols
        objects.append(str(late))
    elif source.name=='SprdMaliService.cpp':
        abi=json.loads((SOURCE/'mali/OriginalAcceleratorABI.json').read_text())
        assert abi['instance_bytes']==136 and abi['virtual_entries']==168 and not abi['operational_overrides']
        bindings={
            '__ZN15IOAcceleratorESC2EPK11OSMetaClass':int(abi['protected_compatible_constructor_aliases'][0],16),
            '__ZN15IOAcceleratorESD2Ev':int(abi['nondeleting_destructor'],16),
            '__ZN15IOAcceleratorES10gMetaClassE':int(abi['metaclass'],16),
        }
        undefined={line.split()[-1] for line in subprocess.check_output(
            ['xcrun','nm','-u',obj],text=True).splitlines() if line.split()}
        used=undefined & set(bindings)
        extra={name for name in undefined if 'IOAcceleratorES' in name and name not in bindings}
        if not used or extra:
            raise SystemExit('Unexpected or missing original IOAcceleratorES imports: '+repr(sorted(undefined)))
        asm=OUT/'accelerator-original-bindings.S'
        asm.write_text(''.join(f'.globl {name}\n.set {name}, {address:#x}\n'
            for name,address in sorted(bindings.items()) if name in used))
        clang=record['arguments'][0]
        binding_obj=OUT/'accelerator-original-bindings.o'
        subprocess.run([clang,'-arch','arm64','-target','arm64-apple-ios17.0','-c',
            str(asm),'-o',str(binding_obj)],cwd=record['directory'],check=True)
        llvm=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin'
        late=OUT/'SprdMaliService.late.o'
        subprocess.run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func',
            '--globalize-symbol=__GLOBAL__sub_I_SprdMaliService.cpp',obj,late],check=True)
        symbols=subprocess.check_output(['xcrun','nm',late],text=True)
        assert ' T __GLOBAL__sub_I_SprdMaliService.cpp' in symbols
        objects.extend([str(late),str(binding_obj)])
        (OUT/'accelerator-abi.json').write_text(json.dumps({
            'source_object_sha256':hashlib.sha256(obj.read_bytes()).hexdigest(),
            'late_object_sha256':hashlib.sha256(late.read_bytes()).hexdigest(),
            'bindings':{name:hex(address) for name,address in bindings.items() if name in used},
            'original_instructions_modified':False,
            'initializer_deferred_until_iogpufamily_load':True,
            'system_process_grants':['backboardd','SpringBoard','WidgetRenderer_','WidgetRenderer_D']},indent=2)+'\n')
    else:objects.append(str(obj))
# The reused kernel-control/Skywalk transport is C, not a C++ IOKit object.
# Use the kernel's real C record. The C++ record contains compact -xc++ and
# language-specific driver options, so translating its flag list is fragile.
source=SOURCE/'SprdBluetoothTransport.c';obj=OUT/'SprdBluetoothTransport.o'
c_records=list((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837').rglob('kern_control.o.json'))
assert len(c_records)==1,c_records
c_record=json.loads(c_records[0].read_text().strip().rstrip(','))
assert Path(c_record['file']).name=='kern_control.c'
command=[];args=c_record['arguments'];i=0
while i<len(args):
    value=args[i]
    if value==c_record['file']:command.append(str(source))
    elif value=='-o':command+=['-o',str(obj)];i+=1
    elif value=='-Xclang' and args[i+1].startswith('-W'):i+=1
    elif value.startswith('-W'):pass
    else:command.append(value)
    i+=1
command+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter']
(OUT/'SprdBluetoothTransport-command.json').write_text(json.dumps(command,indent=2)+'\n')
r=subprocess.run(command,cwd=c_record['directory'],capture_output=True,text=True)
(OUT/'SprdBluetoothTransport.log').write_text(r.stdout+r.stderr)
if r.returncode:raise SystemExit(json.dumps(command)+'\n'+(r.stdout+r.stderr)[-16000:])
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
