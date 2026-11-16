#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Adapted from the pinned UniXNU build-sheng-hid-input.py; no Qualcomm backend.
import hashlib,json,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/sprd-input';OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho

def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=120,**kw)
def require(value,why):
    if not value:raise SystemExit(why)
audit=json.loads((ROOT/'drivers/SprdInput/original-abi.json').read_text())
methods=audit['methods'];names=audit['native_symbols'];vptr=int(audit['vptr'],16)
require(audit['passed'] and audit['slots']==207 and audit['class_bytes']==208,'Qualified original IOHIDDevice audit')
header='''// Generated from the qualified original image; opaque private storage only.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice : public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
'''
for i in range(168,207):
    if i==171:header+=' virtual OSString *newTransportString() const;\n'
    elif i==173:header+=' virtual OSString *newProductString() const;\n'
    elif i==180:header+=' virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0;\n'
    elif i==181:header+=' virtual IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits);\n'
    else:header+=f' virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque{i}();\n'
header+='};\nstatic_assert(sizeof(IOHIDDevice)==0xd0,"Original device instance size");\n'
(OUT/'OriginalHidDeviceABI.h').write_text(header)
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeHidInput'],text=True),end='')
obj=OUT/'NativeHidInput.o';raw=obj.read_bytes();om=macho(raw)
st=next(p for c,p,_ in om['commands'] if c==2);sy,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
    ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,sy+i*16)
    symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
vt=next(va for name,va,_ in symbols if name=='__ZTV14NativeHidInput');relocations={}
for segment in om['segments']:
    for sec in segment['sections']:
        if sec['va']<=vt<sec['va']+sec['size']:
            ro,rc=struct.unpack_from('<2I',raw,sec['command_offset']+56)
            for i in range(rc):
                offset,bits=struct.unpack_from('<2I',raw,ro+i*8)
                if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+offset]=symbols[bits&0xffffff][0]
compiled=[relocations.get(vt+16+i*8) for i in range(207)]
require(all(compiled) and vt+16+207*8 not in relocations,'Compiled exact 207-slot vtable')
bindings={'__ZN11IOHIDDeviceC2EPK11OSMetaClass':0xfffffff004ad94f8,'__ZN11IOHIDDeviceD2Ev':0xfffffff004ad9518,'__ZN11IOHIDDevice10gMetaClassE':0xfffffff006ae6fa0,'__ZTV11IOHIDDevice':vptr-16}
owned=[]
for i,name in enumerate(compiled):
    if name.startswith(('__ZN14NativeHidInput','__ZNK14NativeHidInput')):owned.append(i)
    elif '11IOHIDDevice' in name:bindings[name]=methods[i]
    else:require(i<168 and names.get(name)==methods[i],'Actual inherited native method '+str(i))
require(owned==[0,1,7,14,86,87,111,171,173,180],'Only qualified HID methods overridden')
# This kernel compiler resolves qualified super calls through the original vtable.
undefined={n for n,_,k in symbols if k==1 and '11IOHIDDevice' in n}
require(undefined==set(bindings),'All and only actual original HID imports bound')
asm=OUT/'original-bindings.S';asm.write_text(''.join(f'.globl {n}\n.set {n}, {a:#x}\n' for n,a in sorted(bindings.items())))
sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-c',asm,'-o',OUT/'hid-original-bindings.o'])
llvm=Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin';late=OUT/'NativeHidInput.late.o'
run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeHidInput.cpp',obj,late])
def code(data,image):
    s=next(s for g in image['segments'] for s in g['sections'] if s['name']=='__text');return data[s['fileoff']:s['fileoff']+s['size']]
require(code(raw,om)==code(late.read_bytes(),macho(late.read_bytes())),'Owned code unchanged by deferred metaclass registration')
(OUT/'abi-audit.json').write_text(json.dumps({'reference_candidate_sha256':audit['kernel_sha256'],'original_text_sha256':'2031.120.4','original_slots':207,'original_instance_size':208,'owned_slots':owned,'bindings':bindings,'runtime_qualified':False,'source_sha256':hashlib.sha256((ROOT/'drivers/SprdInput/NativeHidInput.cpp').read_bytes()).hexdigest()},indent=2)+'\n')
print('PASS: original HID instruction identity, base imports, call boundaries and exact owned vtable')

archive=OUT/'libSprdInput.a';archive.unlink(missing_ok=True)
run(['xcrun','ar','rcs',archive,late,OUT/'hid-original-bindings.o'])
require(archive.stat().st_size>0,'Late-init driver archive exists')
