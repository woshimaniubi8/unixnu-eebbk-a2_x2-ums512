#!/usr/bin/env python3
"""Strip only non-exported symbols from the signed owned Metal bundle."""
import argparse,hashlib,importlib.util,json,plistlib,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
def run(args):return subprocess.run(list(map(str,args)),check=True,capture_output=True).stdout
def main(path):
    parser=module('probe_signing',ROOT/'scripts/resign-mali-metal-probe.py')
    packer=module('metal_packer',ROOT/'scripts/make-mali-metal-addon.py')
    probe=path/'viva-gpu/ios/out/tests/native-metal-probe'
    bundle=path/'viva-gpu/ios/out/VivaMTLDriver.bundle';binary=bundle/'VivaMTLDriver'
    assert hashlib.sha256(probe.read_bytes()).hexdigest()=='20e6d7a77d1919b3ddac7a4de8830310ac0848f5c4ee60f0fb1205d85e754dd8'
    before=binary.read_bytes();assert hashlib.sha256(before).hexdigest()=='138aedc22573487105b3071330cee199124c31d62096c6b689c6b837bf309085'
    original=packer.code_directory(before,False);layout=parser.layout(before)
    exports=sorted(run(['xcrun','nm','-gjU',binary]).splitlines());dependencies=run(['xcrun','otool','-L',binary])
    run(['xcrun','strip','-S','-x',binary])
    with tempfile.TemporaryDirectory() as temporary:
        command=['codesign','--force','--sign','-','--pagesize',str(original['page_bytes']),'--identifier',layout['identifier']]
        if original['entitlements'] is not None:
            entitlements=Path(temporary)/'bundle-entitlements.plist';entitlements.write_bytes(plistlib.dumps(original['entitlements']))
            command+=['--entitlements',entitlements]
        run([*command,bundle])
    run(['codesign','--verify','--strict',bundle]);run(['codesign','--verify','--strict',probe])
    after=binary.read_bytes();modified=packer.code_directory(after,False);new_layout=parser.layout(after)
    assert new_layout['sections']==layout['sections'] and new_layout['header_identity']==layout['header_identity']
    assert new_layout['flags']==layout['flags'] and new_layout['identifier']==layout['identifier']
    assert sorted(run(['xcrun','nm','-gjU',binary]).splitlines())==exports
    assert run(['xcrun','otool','-L',binary])==dependencies
    for key in ['entitlements','flags','page_bytes']:assert original[key]==modified[key]
    assert len(before)-len(after)>=400000,('insufficient actual symbol savings',len(before),len(after))
    assert hashlib.sha256(probe.read_bytes()).hexdigest()=='20e6d7a77d1919b3ddac7a4de8830310ac0848f5c4ee60f0fb1205d85e754dd8'
    signature=subprocess.run(['codesign','-d','--verbose=4',bundle],capture_output=True,text=True,check=True)
    (path/'signature.txt').write_text(signature.stdout+signature.stderr)
    receipt=dict(compiled_source_run=44743450966,compiled_source_commit='80a136ce57b5a6ad9774ce918ae8c5f113ea8a74',parent_preparation_run=44772957799,
        original_bundle_sha256=hashlib.sha256(before).hexdigest(),modified_bundle_sha256=hashlib.sha256(after).hexdigest(),
        original_bytes=len(before),modified_bytes=len(after),saved_bytes=len(before)-len(after),
        original_signature=original,modified_signature=modified,program_bytes_unchanged=True,
        all_code_and_data_sections_unchanged=True,exported_symbols_unchanged=True,dependencies_unchanged=True,
        identifier_and_entitlements_unchanged=True,tool='Apple xcrun strip -S -x; codesign exact original bundle identifier/entitlements/page-size')
    (path/'deployment-preparation.json').write_text(json.dumps(receipt,indent=2)+'\n')
    files=[*bundle.rglob('*'),probe,probe.with_suffix('.metallib'),path/'viva-gpu/g52-source-receipt.json',path/'deployment-preparation.json']
    (path/'SHA256SUMS').write_text('\n'.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(path).as_posix() for p in sorted(files) if p.is_file())+'\n')
    print(json.dumps(receipt,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('artifact',type=Path);a=p.parse_args();main(a.artifact.resolve())
