#!/usr/bin/env python3
"""Qualify an M69-only opt-in full process-exit description diagnostic."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

INPUT_SHA='1e783af89d45ca80f5eeaf4505b7a3a2568253be6269dbbfad3bfe6744189bec'
OLD_REASON=b'pid: %d -- process name: %s -- exit reason namespace: %d -- subcode: 0x%llx -- description: %s\0'
NEW_REASON=b'bbk exit: pid=%d name=%s ns=%d code=0x%llx desc=%s\n\0'
OLD_STATUS=b'pid: %d -- process name: %s -- exit status %d\0'
NEW_STATUS=b'bbk exit: pid=%d name=%s status=%d\n\0'


def qualify(data,tools):
    sys.path.insert(0,str(tools))
    from native_prelink import macho,exports,backed_offset
    image=macho(data);symbols=exports(data,image)
    def address(name):
        a=[a for a,names in symbols.items() if name in names]
        assert len(a)==1,(name,a)
        return a[0]
    def read(va,size):
        at=backed_offset(image['segments'],va,size,len(data));return at,data[at:at+size]
    function=address('_proc_prepareexit')
    assert function==0xfffffff0076efc1c
    call=0xfffffff0076efe14
    _,instructions=read(call-8,12)
    assert instructions==bytes.fromhex('2000805281008052c92f0694')
    assert address('_record_system_event')==0xfffffff00787bd38
    target=address('_printf');assert target==0xfffffff0072a83ac
    displacement=target-call
    assert displacement%4==0 and -(1<<27)<=displacement<(1<<27)
    ranges=[]
    def add(va,before,after):
        at,value=read(va,len(before));assert value==before
        ranges.append(dict(va=hex(va),offset=at,old_hex=before.hex(),new_hex=after.hex()))
    # Darwin arm64 variadic arguments are already in the caller's stack area.
    # x3 holds its format; change only x0 to that format and the call target.
    add(call-8,bytes.fromhex('20008052'),bytes.fromhex('e00303aa')) # mov x0,x3
    add(call,instructions[8:],struct.pack('<I',0x94000000|((displacement//4)&0x3ffffff)))
    for va,old,new in ((0xfffffff0070cc0ab,OLD_REASON,NEW_REASON),
                       (0xfffffff0070cc10a,OLD_STATUS,NEW_STATUS)):
        assert data.count(old)==1 and len(new)<=len(old)
        add(va,old,new+bytes(len(old)-len(new)))
    # Both branches set x3 and the existing stack arguments before this shared
    # call site. Retain the existing exception_log_max_pid comparison entirely.
    _,before=read(call-0x50,0x50)
    assert before.endswith(bytes.fromhex('2000805281008052'))
    return dict(function_va=hex(function),call_va=hex(call),printf_va=hex(target),ranges=ranges)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernel',type=Path);p.add_argument('--prelink-tools',type=Path,required=True)
    p.add_argument('--output',type=Path);p.add_argument('--report',type=Path)
    a=p.parse_args();source=a.kernel.read_bytes()
    assert hashlib.sha256(source).hexdigest()==INPUT_SHA,'Only the qualified M69 input is accepted'
    report=qualify(source,a.prelink_tools);result=bytearray(source);allowed=set()
    for entry in report['ranges']:
        at=entry['offset'];old=bytes.fromhex(entry['old_hex']);new=bytes.fromhex(entry['new_hex'])
        result[at:at+len(old)]=new;allowed.update(range(at,at+len(old)))
    changed={i for i,(a,b) in enumerate(zip(source,result)) if a!=b}
    assert len(source)==len(result) and changed<=allowed
    report.update(input_sha256=INPUT_SHA,output_sha256=hashlib.sha256(result).hexdigest(),
                  changed_bytes=len(changed),all_other_bytes_unchanged=True,
                  exit_and_watchdog_policy_unchanged=True,hardware_tested=False)
    if a.output:
        assert not a.output.exists() and a.output.resolve()!=a.kernel.resolve()
        a.output.write_bytes(result)
    if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
