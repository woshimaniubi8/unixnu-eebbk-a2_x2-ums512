#!/usr/bin/env python3
"""Rebuild only the owned probe, retaining the qualified stripped bundle."""
import argparse,hashlib,importlib.util,json,plistlib,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def run(args):return subprocess.run(list(map(str,args)),check=True,capture_output=True).stdout
def main(path):
    s=importlib.util.spec_from_file_location('packer',ROOT/'scripts/make-mali-metal-addon.py');packer=importlib.util.module_from_spec(s);s.loader.exec_module(packer)
    probe=path/'viva-gpu/ios/out/tests/native-metal-probe';bundle=path/'viva-gpu/ios/out/VivaMTLDriver.bundle';binary=bundle/'VivaMTLDriver'
    old_sha=hashlib.sha256(probe.read_bytes()).hexdigest();assert old_sha=='022ac8915952ca011a14586afbc80e6c36e0460c74296aa4a7cc19ae28f2eb46'
    original=packer.code_directory(probe.read_bytes(),True)
    bundle_sha=hashlib.sha256(binary.read_bytes()).hexdigest();assert bundle_sha=='bb14ae745801838d447d8e5ce0cfec1768d3e45bd1787f38e22fd3065e31bdbd'
    source=ROOT/'drivers/SprdPlatform/userspace/native-metal-probe.m';ent=source.with_suffix('.entitlements')
    sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path']).decode().strip()
    clang=run(['xcrun','--sdk','iphoneos','-f','clang']).decode().strip()
    command=[clang,'-target','arm64-apple-ios17.0','-isysroot',sdk,'-fobjc-arc','-O2','-Wall','-Wextra','-Werror',source,'-framework','Foundation','-framework','Metal','-framework','IOSurface','-o',probe]
    run(command)
    identifier='native-metal-probe-5555494489c159357fe23e76b7d9b3b59930e92b'
    run(['codesign','--force','--sign','-','--pagesize','4096','--identifier',identifier,'--entitlements',ent,probe])
    run(['codesign','--verify','--strict',probe]);run(['codesign','--verify','--strict',bundle])
    signed=packer.code_directory(probe.read_bytes(),True)
    assert signed['entitlements']==original['entitlements']==plistlib.loads(ent.read_bytes())
    assert signed['flags']==original['flags'] and signed['page_bytes']==4096
    assert hashlib.sha256(binary.read_bytes()).hexdigest()==bundle_sha
    detail=subprocess.run(['codesign','-d','--verbose=4',probe],capture_output=True,text=True,check=True)
    (path/'native-metal-signature.txt').write_text(detail.stdout+detail.stderr)
    record=dict(probe_recompiled_only=True,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),original_probe_sha256=old_sha,
        probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),probe_signature=signed,
        bundle_sha256=bundle_sha,bundle_bytes_unchanged=True,shader_bytes_unchanged=True,
        unchanged_shader_sha256=hashlib.sha256(probe.with_suffix('.metallib').read_bytes()).hexdigest(),
        diagnostics='original IOSurface metadata, private temporary-fd contract and direct native14 pin/free before unchanged Mesa/Metal pipelines',command=list(map(str,command)))
    (path/'probe-trace-build.json').write_text(json.dumps(record,indent=2)+'\n')
    files=[*bundle.rglob('*'),probe,probe.with_suffix('.metallib'),path/'viva-gpu/g52-source-receipt.json',path/'probe-trace-build.json']
    (path/'SHA256SUMS').write_text('\n'.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(path).as_posix() for p in sorted(files) if p.is_file())+'\n')
    print(json.dumps(record,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('artifact',type=Path);a=p.parse_args();main(a.artifact.resolve())
