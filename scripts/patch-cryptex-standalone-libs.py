#!/usr/bin/env python3
"""Qualify the M70 final Cryptex probe -> stock standalone-library graft."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

INPUT_SHA='d68f2a479f9c7a799c9ac4f571e02f9dde932a43ce539c33d88c658f24171590'
START=0xfffffff0076ebac8
END=0xfffffff0076ebaf4
BIND=0xfffffff0076ecf04
ERROR=0xfffffff0076ebafc
STRING=0xfffffff0070cbaf7
OLD_STRING=b'/System/Cryptexes/OS/System/Library/ExtensionKit/Extensions/WebContentExtension.appex/Info.plist\0'
SOURCE=b'/private/preboot/usr/lib\0'
TARGET=b'/usr/lib\0'
OLD_CODE=bytes.fromhex('62d9f697e30300aa00cfff9000dc2b91e2430591010080527247f697c0000035e0ab40f9b13ef69700008052')


def adrp(reg,pc,target):
    pages=(target>>12)-(pc>>12);assert -(1<<20)<=pages<(1<<20)
    immediate=pages&0x1fffff
    return 0x90000000|((immediate&3)<<29)|((immediate>>2)<<5)|reg


def code():
    values=[0xaa1903e0,0xaa1403e1, # x0=proc (x25), x1=user scratch (x20)
            adrp(2,START+8,STRING),0x91000042|((STRING&4095)<<10),
            adrp(3,START+16,STRING+len(SOURCE)),0x91000063|(((STRING+len(SOURCE))&4095)<<10),
            0x52800404] # mov w4,#MNT_UNION (0x20); helper adds MNT_RDONLY
    offset=BIND-(START+28);assert offset%4==0 and -(1<<27)<=offset<(1<<27)
    values.append(0x94000000|((offset//4)&0x3ffffff))
    offset=ERROR-(START+32);assert offset%4==0 and -(1<<20)<=offset<(1<<20)
    values += [0x35000000|(((offset//4)&0x7ffff)<<5),0xd503201f,0xd503201f]
    result=struct.pack('<11I',*values);assert len(result)==END-START
    return result


def qualify(data,tools):
    sys.path.insert(0,str(tools))
    from native_prelink import macho,backed_offset
    image=macho(data)
    def read(va,n):
        at=backed_offset(image['segments'],va,n,len(data));return at,data[at:at+n]
    at,original=read(START,END-START);assert original==OLD_CODE
    # The existing stage=4 entry and stage=5/status epilogue are retained.
    assert read(START-8,8)[1]==bytes.fromhex('88008052c8a20bb9')
    assert read(END,16)[1]==bytes.fromhex('a8008052c8a20bb9481b00b000dd0fb9')
    string_at,string=read(STRING,len(OLD_STRING));assert string==OLD_STRING and data.count(OLD_STRING)==1
    new_string=SOURCE+TARGET;assert len(new_string)<=len(OLD_STRING)
    return [dict(va=hex(START),offset=at,old_hex=original.hex(),new_hex=code().hex()),
            dict(va=hex(STRING),offset=string_at,old_hex=OLD_STRING.hex(),
                 new_hex=(new_string+bytes(len(OLD_STRING)-len(new_string))).hex())]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernel',type=Path);p.add_argument('--prelink-tools',type=Path,required=True)
    p.add_argument('--output',type=Path);p.add_argument('--report',type=Path)
    a=p.parse_args();source=a.kernel.read_bytes();assert hashlib.sha256(source).hexdigest()==INPUT_SHA
    ranges=qualify(source,a.prelink_tools);result=bytearray(source);allowed=set()
    for entry in ranges:
        at=entry['offset'];old=bytes.fromhex(entry['old_hex']);new=bytes.fromhex(entry['new_hex'])
        result[at:at+len(old)]=new;allowed.update(range(at,at+len(old)))
    changed={i for i,(x,y) in enumerate(zip(source,result)) if x!=y}
    assert len(source)==len(result) and changed<=allowed
    report=dict(input_sha256=INPUT_SHA,output_sha256=hashlib.sha256(result).hexdigest(),ranges=ranges,
                changed_bytes=len(changed),all_other_bytes_unchanged=True,
                source=SOURCE.rstrip(b'\0').decode(),target=TARGET.rstrip(b'\0').decode(),
                mount_flags='MNT_RDONLY | MNT_UNION',existing_bind_helper_va=hex(BIND),
                stage5_meaning='standalone Cryptex usr/lib graft mounted successfully',
                former_webcontent_probe_removed=True,hardware_tested=False)
    if a.output:
        assert not a.output.exists() and a.output.resolve()!=a.kernel.resolve();a.output.write_bytes(result)
    if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
