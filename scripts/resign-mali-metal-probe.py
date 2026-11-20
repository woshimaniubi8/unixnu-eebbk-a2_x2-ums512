#!/usr/bin/env python3
"""Re-sign only the owned probe; preserve its code, identity and permissions."""
import argparse,hashlib,importlib.util,json,plistlib,struct,subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
PARENT_RUN=44743450966
PARENT_COMMIT='80a136ce57b5a6ad9774ce918ae8c5f113ea8a74'
PROBE_SHA='78ff453f01076e66e580e39bfb672150d8d96aa583c671fd011f8b186a708a7a'
BUNDLE_SHA='138aedc22573487105b3071330cee199124c31d62096c6b689c6b837bf309085'

def layout(data):
    assert struct.unpack_from('<II',data)==(0xfeedfacf,0x100000c)
    n=struct.unpack_from('<I',data,16)[0];at=32;sections={};commands=[];identifier=None
    for _ in range(n):
        command,size=struct.unpack_from('<II',data,at);assert size>=8
        if command==0x19:
            name,va,span,offset,length,maximum,initial,nsec=struct.unpack_from('<16s4Q3I',data,at+8)
            name=name.rstrip(b'\0').decode()
            if name!='__LINKEDIT':commands.append(data[at:at+size])
            for k in range(nsec):
                sec=at+72+80*k;sn,sg,addr,sz,off=struct.unpack_from('<16s16sQQI',data,sec)
                flags=struct.unpack_from('<I',data,sec+64)[0]
                if flags&0xff in (1,0xc,0x12):contents=bytes(sz)
                else:contents=data[off:off+sz]
                sections[name+'/'+sn.rstrip(b'\0').decode()]=hashlib.sha256(contents).hexdigest()
        elif command==0x1d:
            off,length=struct.unpack_from('<II',data,at+8);signature=data[off:off+length]
            for j in range(struct.unpack_from('>I',signature,8)[0]):
                slot,part=struct.unpack_from('>II',signature,12+8*j)
                magic,sz=struct.unpack_from('>II',signature,part)
                if magic==0xfade0c02:
                    cd=signature[part:part+sz];ident=struct.unpack_from('>I',cd,20)[0]
                    identifier=cd[ident:].split(b'\0')[0].decode()
        else:commands.append(data[at:at+size])
        at+=size
    assert identifier
    return dict(header_identity=data[:16].hex(),flags=data[24:28].hex(),sections=sections,
        commands_sha256=hashlib.sha256(b''.join(commands)).hexdigest(),identifier=identifier)

def main(path):
    probe=path/'viva-gpu/ios/out/tests/native-metal-probe'
    bundle=path/'viva-gpu/ios/out/VivaMTLDriver.bundle/VivaMTLDriver'
    before=probe.read_bytes();assert hashlib.sha256(before).hexdigest()==PROBE_SHA
    assert hashlib.sha256(bundle.read_bytes()).hexdigest()==BUNDLE_SHA
    spec=importlib.util.spec_from_file_location('packer',ROOT/'scripts/make-mali-metal-addon.py')
    packer=importlib.util.module_from_spec(spec);spec.loader.exec_module(packer)
    old=packer.code_directory(before,True);assert old['page_bytes']==16384
    old_layout=layout(before);ent=ROOT/'drivers/SprdPlatform/userspace/native-metal-probe.entitlements'
    assert old['entitlements']==plistlib.loads(ent.read_bytes())
    command=['codesign','--force','--sign','-','--pagesize','4096','--identifier',old_layout['identifier'],'--entitlements',str(ent),str(probe)]
    subprocess.run(command,check=True);subprocess.run(['codesign','--verify','--strict',str(probe)],check=True)
    after=probe.read_bytes();new=packer.code_directory(after,True)
    assert new['page_bytes']==4096 and new['entitlements']==old['entitlements'] and new['flags']==old['flags']
    assert layout(after)==old_layout
    assert hashlib.sha256(bundle.read_bytes()).hexdigest()==BUNDLE_SHA
    details=subprocess.run(['codesign','-d','--verbose=4',str(probe)],capture_output=True,text=True,check=True)
    (path/'native-metal-signature.txt').write_text(details.stdout+details.stderr)
    receipt=dict(signing_only=True,compiled_source_run=PARENT_RUN,compiled_source_commit=PARENT_COMMIT,
        original_sha256=PROBE_SHA,modified_sha256=hashlib.sha256(after).hexdigest(),
        original_signature=old,modified_signature=new,unchanged_loads_code_and_data=True,
        unchanged_identifier=old_layout['identifier'],bundle_bytes_unchanged=True,
        changed_property='probe CodeDirectory hash page:16KiB to4KiB',command=command)
    (path/'signing-only.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
    bundle_root=bundle.parent
    files=[*bundle_root.rglob('*'),probe,probe.with_suffix('.metallib'),
           path/'viva-gpu/g52-source-receipt.json',path/'signing-only.json']
    (path/'SHA256SUMS').write_text('\n'.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(path).as_posix()
        for p in sorted(files) if p.is_file())+'\n')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('artifact',type=Path);a=p.parse_args();main(a.artifact.resolve())
