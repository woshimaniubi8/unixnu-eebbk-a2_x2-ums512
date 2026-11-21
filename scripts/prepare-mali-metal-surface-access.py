#!/usr/bin/env python3
"""Re-sign only the exact M188 owner probe after a measured IOSurface MACF denial."""
import argparse,hashlib,importlib.util,json,plistlib,subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
PROBE='e54f42034eb561940831e5be54870ebb3dcfd6a1ddea1722cb18155c9eb2901e'
BUNDLE='bb14ae745801838d447d8e5ce0cfec1768d3e45bd1787f38e22fd3065e31bdbd'
SHADER='e258239d4cf086ab92577ba9ee73e886f7aa55760710c5e0f0380bbf2332faf0'

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main(path):
    packer=module('metal_packer',ROOT/'scripts/make-mali-metal-addon.py')
    signing=module('metal_signing',ROOT/'scripts/resign-mali-metal-probe.py')
    probe=path/'viva-gpu/ios/out/tests/native-metal-probe'
    bundle=path/'viva-gpu/ios/out/VivaMTLDriver.bundle'
    binary=bundle/'VivaMTLDriver';shader=probe.with_suffix('.metallib')
    assert sha(probe)==PROBE and sha(binary)==BUNDLE and sha(shader)==SHADER
    ent=ROOT/'drivers/SprdPlatform/userspace/native-metal-probe.entitlements'
    wanted=plistlib.loads(ent.read_bytes())
    old_ent={**wanted,'com.apple.security.iokit-user-client-class':['NativeSprdMaliUserClient']}
    assert wanted=={**old_ent,'com.apple.security.iokit-user-client-class':['NativeSprdMaliUserClient','IOSurfaceRootUserClient']}
    before=probe.read_bytes();old_layout=signing.layout(before)
    original=packer.code_directory(before,True,expected_entitlements=old_ent)
    command=['codesign','--force','--sign','-','--pagesize','4096','--identifier',old_layout['identifier'],
             '--entitlements',str(ent),str(probe)]
    subprocess.run(command,check=True)
    for target in [probe,bundle]:subprocess.run(['codesign','--verify','--strict',str(target)],check=True)
    after=probe.read_bytes();modified=packer.code_directory(after,True)
    assert modified['entitlements']==wanted and modified['page_bytes']==4096
    assert modified['flags']==original['flags'] and signing.layout(after)==old_layout
    assert sha(binary)==BUNDLE and sha(shader)==SHADER
    details=subprocess.run(['codesign','-d','--verbose=4','--entitlements',':-',str(probe)],capture_output=True,text=True,check=True)
    (path/'native-metal-signature.txt').write_text(details.stdout+details.stderr)
    record=dict(signing_only=True,parent_frontend_run=44838691910,
        parent_frontend_source_commit='055104966d99cc6b6c642c8ad5a26bd8bd2d3046',
        original_probe_sha256=PROBE,probe_sha256=sha(probe),original_signature=original,probe_signature=modified,
        unchanged_loads_code_and_data=True,unchanged_identifier=old_layout['identifier'],
        bundle_sha256=BUNDLE,bundle_bytes_unchanged=True,shader_sha256=SHADER,shader_bytes_unchanged=True,
        changed_property='Only this signed owner probe may open original IOSurfaceRootUserClient; actual M188 MACF denial observed.',
        command=command)
    receipt=path/'surface-access-signing.json';receipt.write_text(json.dumps(record,indent=2)+'\n')
    files=[*bundle.rglob('*'),probe,shader,path/'viva-gpu/g52-source-receipt.json',receipt]
    (path/'SHA256SUMS').write_text('\n'.join(sha(p)+'  '+p.relative_to(path).as_posix() for p in sorted(files) if p.is_file())+'\n')
    print(json.dumps(record,indent=2))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('artifact',type=Path)
    main(parser.parse_args().artifact.resolve())
