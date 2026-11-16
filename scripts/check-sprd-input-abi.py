#!/usr/bin/env python3
"""Guard the original 21H461 IOHIDDevice ABI; never infer support by strings."""
import argparse, hashlib, json, struct, sys
from pathlib import Path

VPTR=0xfffffff005d045c0
OVERRIDES=[0,1,7,12,14,21,23,76,81,82,86,87,91,92,93,101,132,140]
WINDOWS={0xfffffff004ad94f8:32,0xfffffff004ad9518:32,
         0xfffffff004ad9aec:52,0xfffffff004add110:28,
         0xfffffff005d00f10:16}
def check(path,tools,reference=None):
    sys.path.insert(0,str(tools))
    from native_prelink import macho,backed_offset,exports,unserialize_xml
    data=Path(path).read_bytes();m=macho(data)
    def read(a,n):
        o=backed_offset(m['segments'],a,n,len(data));return data[o:o+n]
    names={n:a for a,ns in exports(data,m).items() for n in ns}
    info=next(s for s in m['segments'] if s['name']=='__PRELINK_INFO')
    entries=unserialize_xml(read(info['va'],info['filesize']))['_PrelinkInfoDictionary']
    hid=next(e for e in entries if e['CFBundleIdentifier']=='com.apple.iokit.IOHIDFamily')
    assert 'IOHIDFamily-2031.120.4' in hid['NSHumanReadableCopyright']
    methods=struct.unpack('<207Q',read(VPTR,207*8));base=struct.unpack('<168Q',read(names['__ZTV9IOService']+16,168*8))
    assert [i for i in range(168) if methods[i]!=base[i]]==OVERRIDES
    assert [i for i,a in enumerate(methods) if a==names['___cxa_pure_virtual']]==[180]
    assert read(VPTR+207*8,24)==bytes(24)
    assert read(0xfffffff004ad94f8,32)==bytes.fromhex('fd7bbfa9fd0300912bd90094489100f008011791080000f9fd7bc1a8c0035fd6')
    assert struct.unpack('<Q',read(0xfffffff005d00f10,8))[0]==names['__ZN9IOServiceC2EPK11OSMetaClass']
    assert struct.unpack('<Q',read(0xfffffff005d00f18,8))[0]==names['__ZN9IOServiceD2Ev']
    assert read(0xfffffff004ad9aec,52)==bytes.fromhex('680240f908a142f9e00313aae10314aa00013fd6680240f9603d003608d142f9e1630091e00313aa00013fd6a02d0035e00f40f9')
    assert methods[181]==0xfffffff004add0c4
    assert read(0xfffffff004add110,28)==bytes.fromhex('680240f9081543f9e00313aae20316aae30315aae40314aa00013fd6')
    # Constructor directly supplies the public metaclass address and original
    # class vptr. The private instance extent is additionally checked at boot.
    record=dict(kernel_sha256=hashlib.sha256(data).hexdigest(),vptr=hex(VPTR),slots=207,class_bytes=208,
                methods=list(methods),native_symbols={n:a for n,a in names.items() if a in base},
                read_windows={hex(a):read(a,n).hex() for a,n in WINDOWS.items()},passed=True)
    if reference:
        old=json.loads(Path(reference).read_text())
        for i in OVERRIDES+list(range(168,207)):
            if i not in [0,1,7,180]:assert methods[i]==old['methods'][i],('original HID method moved',i)
        for a,n in WINDOWS.items():
            if a!=0xfffffff005d00f10:assert read(a,n).hex()==old['read_windows'][hex(a)],hex(a)
    return record

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('kernel',type=Path)
    p.add_argument('--prelink-tools',type=Path,required=True);p.add_argument('--write-reference',type=Path)
    a=p.parse_args();ref=Path(__file__).resolve().parents[1]/'drivers/SprdInput/original-abi.json'
    r=check(a.kernel,a.prelink_tools,None if a.write_reference else ref)
    if a.write_reference:a.write_reference.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps({k:v for k,v in r.items() if k not in ['methods','native_symbols','read_windows']},indent=2))
