#!/usr/bin/env python3
"""Qualify/apply the M68-only steady-state temperature-read correction."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

M68_SHA256='502a7517ca3c416fed72de1978f25d06f7e4dcdd55007743647a84c4d208dbec'
FUNCTION='__ZN17NativeSprdThermal4pollEv'
NOP=bytes.fromhex('1f2003d5')


def qualify(data, tools):
    sys.path.insert(0,str(tools))
    from native_prelink import macho, backed_offset, exports
    image=macho(data)
    symbols=exports(data,image)
    functions=[a for a,names in symbols.items() if FUNCTION in names]
    assert len(functions)==1, functions
    function=functions[0]
    end=min(a for a in symbols if a>function)
    start=backed_offset(image['segments'],function,end-function,len(data))
    # Load INTERNAL_STS1, reject if bit 0, then preserve the ADC range test.
    signature=bytes.fromhex('285540b968040037bfa20f7128040054')
    code=data[start:start+end-function]
    assert code.count(signature)==1, 'Unexpected thermal poll instruction layout'
    relative=code.index(signature)
    at=start+relative+4
    address=function+relative+4
    assert address==0xfffffff00798df68 and struct.unpack_from('<I',data,at)[0]==0x37000468
    # Both the bad status branch and the real ADC/slope guards target the
    # existing invalid path. Modify only the status branch, never those guards.
    assert data[at+4:at+24]==bytes.fromhex('bfa20f712804005468c35fb81f050071cb030054')
    return dict(function_va=hex(function),branch_va=hex(address),offset=at,
                old_hex=data[at:at+4].hex(),new_hex=NOP.hex(),
                explanation='Remove periodic INTERNAL_STS1.bit0 rejection after the initial vendor wait.')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernel',type=Path)
    p.add_argument('--prelink-tools',type=Path,required=True)
    p.add_argument('--output',type=Path)
    p.add_argument('--report',type=Path)
    a=p.parse_args()
    source=a.kernel.read_bytes()
    digest=hashlib.sha256(source).hexdigest()
    assert digest==M68_SHA256, 'This correction accepts only the qualified M68 kernel'
    patch=qualify(source,a.prelink_tools)
    result=bytearray(source)
    at=patch['offset']
    result[at:at+4]=NOP
    assert result[:at]==source[:at] and result[at+4:]==source[at+4:]
    report=dict(input_sha256=digest,output_sha256=hashlib.sha256(result).hexdigest(),
                ranges=[patch],changed_bytes=sum(x!=y for x,y in zip(source,result)),
                all_other_bytes_unchanged=True,hardware_tested=False)
    if a.output:
        assert a.output.resolve()!=a.kernel.resolve() and not a.output.exists()
        a.output.write_bytes(result)
    if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
