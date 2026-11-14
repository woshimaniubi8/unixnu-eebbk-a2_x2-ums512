#!/usr/bin/env python3
"""Qualify the HID executable owner; surgically correct the delivered M67 kernel."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

M67='2a25553af90fd6a1af2a720ca5f7d4b1c31adf92c5dd93bb68778a86f96b0082'
OLD=b'com.apple.iokit.IOHIDEventDriver\0'
NEW=b'com.apple.iokit.IOHIDFamily\0'
FUNCTION='__ZN17NativeSprdThermal14publishSensorsEv'


def inspect(data,tools):
    sys.path.insert(0,str(tools))
    from native_prelink import macho,exports,backed_offset
    image=macho(data);symbols=exports(data,image)
    functions=[a for a,names in symbols.items() if FUNCTION in names]
    assert len(functions)==1,'thermal publication function not unique'
    function=functions[0];matches=[]
    def read(va,n):
        at=backed_offset(image['segments'],va,n,len(data));return at,data[at:at+n]
    for delta in range(0,0x60,4):
        pc=function+delta;_,body=read(pc,12);a,b,c=struct.unpack('<3I',body)
        if a&0x9f00001f!=0x90000000 or b&0xffc003ff!=0x91000000 or c&0xfc000000!=0x94000000:continue
        rel=c&0x3ffffff
        if rel&0x2000000:rel-=0x4000000
        target=pc+8+rel*4
        if not any(n.lstrip('_')=='OSKextLoadKextWithIdentifier' for n in symbols.get(target,[])):continue
        imm=((a>>5&0x7ffff)<<2)|(a>>29&3)
        if imm&0x100000:imm-=0x200000
        literal=(pc&~4095)+imm*4096+(b>>10&4095)
        at,raw=read(literal,64)
        matches.append(dict(function_va=hex(function),call_va=hex(pc+8),literal_va=hex(literal),
                            literal_offset=at,identifier=raw.split(b'\0')[0].decode()))
    assert len(matches)==1,'executable-load call not unique'
    return matches[0]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernel',type=Path);p.add_argument('--prelink-tools',type=Path,required=True)
    p.add_argument('--output',type=Path);p.add_argument('--report',type=Path)
    a=p.parse_args();data=a.kernel.read_bytes();before=inspect(data,a.prelink_tools)
    report=dict(before=before,input_sha256=hashlib.sha256(data).hexdigest(),hardware_tested=False)
    if a.output:
        assert a.output.resolve()!=a.kernel.resolve(),'copy-first: output must differ from input'
        assert not a.output.exists(),'output already exists; inspect before writing'
        assert report['input_sha256']==M67,'not the qualified M67 baseline'
        assert before['identifier']==OLD[:-1].decode()
        at=before['literal_offset'];assert data[at:at+len(OLD)]==OLD
        modified=bytearray(data);modified[at:at+len(OLD)]=NEW+bytes(len(OLD)-len(NEW))
        old_log=b'bbk thm: IOHIDEventDriver load failed\n\0'
        new_log=b'bbk thm: IOHIDFamily load failed\n\0'
        assert data.count(old_log)==1
        log_at=data.index(old_log)
        modified[log_at:log_at+len(old_log)]=new_log+bytes(len(old_log)-len(new_log))
        allowed=set(range(at,at+len(OLD)))|set(range(log_at,log_at+len(old_log)))
        changed=[i for i,(x,y) in enumerate(zip(data,modified)) if x!=y]
        assert set(changed)<=allowed and changed and len(modified)==len(data)
        assert inspect(modified,a.prelink_tools)['identifier']==NEW[:-1].decode()
        a.output.write_bytes(modified)
        report.update(output_sha256=hashlib.sha256(modified).hexdigest(),changed_bytes=len(changed),
                      ranges=[dict(offset=at,old_hex=data[at:at+len(OLD)].hex(),new_hex=modified[at:at+len(OLD)].hex()),
                              dict(offset=log_at,old_hex=old_log.hex(),new_hex=modified[log_at:log_at+len(old_log)].hex())],
                      after=inspect(modified,a.prelink_tools),all_other_bytes_unchanged=True)
    else:
        assert before['identifier']==NEW[:-1].decode(),'HID loader still targets the codeless child'
        report['passed']=True
    if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
