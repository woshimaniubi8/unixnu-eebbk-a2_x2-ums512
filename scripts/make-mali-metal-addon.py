#!/usr/bin/env python3
"""Package the verified owner Metal test through the existing NGPUADD2 loader.

The original trust modules and system binaries remain byte-for-byte intact.
The addon launches one signed diagnostic, not a published system accelerator.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import sys
import uuid
import zlib

ROOT=Path(__file__).resolve().parents[1]
ENTITLEMENTS={
    'com.apple.private.security.no-container':True,
    'com.apple.security.iokit-user-client-class':['NativeSprdMaliUserClient'],
}

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def code_directory(data,executable):
    if len(data)<32 or struct.unpack_from('<II',data)!=(0xfeedfacf,0x100000c):
        raise ValueError('Require a thin ARM64 Mach-O')
    kind,count,commands=struct.unpack_from('<III',data,12)
    if kind!=(2 if executable else 8) or commands>len(data)-32:
        raise ValueError('Unexpected Mach-O kind or load commands')
    if executable:
        layout=module('owner_probe_layout',ROOT/'scripts/prepare-mali-probe-boot.py')
        layout.validate_layout(data)
    at=32;signature=None
    for _ in range(count):
        command,size=struct.unpack_from('<II',data,at)
        if size<8 or at+size>32+commands:raise ValueError('Invalid load command')
        if command==0x1d:
            if signature is not None or size!=16:raise ValueError('Ambiguous signature')
            offset,length=struct.unpack_from('<II',data,at+8)
            if offset>len(data) or length>len(data)-offset:raise ValueError('Signature exceeds file')
            signature=data[offset:offset+length]
        at+=size
    if at!=32+commands or signature is None:raise ValueError('Missing embedded signature')
    magic,length,count=struct.unpack_from('>III',signature)
    if magic!=0xfade0cc0 or length>len(signature) or count>(length-12)//8:
        raise ValueError('Invalid signature superblob')
    directories=[];entitlements=None
    for i in range(count):
        slot,offset=struct.unpack_from('>II',signature,12+i*8)
        if offset>length-8:raise ValueError('Invalid signature index')
        magic,size=struct.unpack_from('>II',signature,offset)
        if size<8 or size>length-offset:raise ValueError('Invalid signature blob')
        blob=signature[offset:offset+size]
        if magic==0xfade7171:
            if entitlements is not None:raise ValueError('Duplicate entitlements')
            entitlements=plistlib.loads(blob[8:])
        if magic!=0xfade0c02:continue
        version,flags,hash_offset,_,_,slots,limit=struct.unpack_from('>7I',blob,8)
        hash_size,hash_type,_,shift=struct.unpack_from('4B',blob,36)
        if version>=0x20300:
            limit64=struct.unpack_from('>Q',blob,56)[0]
            if limit64:limit=limit64
        if hash_type!=2 or hash_size!=32 or shift not in (12,14) or limit>len(data):
            raise ValueError('Require SHA-256 code pages of 4 or 16 KiB')
        page=1<<shift
        if slots!=(limit+page-1)//page or hash_offset>len(blob) or slots*32>len(blob)-hash_offset:
            raise ValueError('Invalid code page table')
        for n in range(slots):
            if hashlib.sha256(data[n*page:min((n+1)*page,limit)]).digest()!=blob[hash_offset+n*32:hash_offset+(n+1)*32]:
                raise ValueError('Code page digest mismatch')
        directories.append(dict(cdhash=hashlib.sha256(blob).hexdigest()[:40],page_bytes=page,code_pages=slots,flags=flags))
    if len(directories)!=1:raise ValueError('Require one unambiguous SHA-256 CodeDirectory')
    if executable and entitlements!=ENTITLEMENTS:raise ValueError('Wrong owner-client entitlement set')
    return dict(**directories[0],entitlements=entitlements)

def encode(files,pad):
    body=bytearray();seen=set()
    for name,mode,data in files:
        if name in seen or name.startswith('/') or '..' in name or '\\' in name:raise ValueError('Invalid addon path')
        seen.add(name);encoded=name.encode()+b'\0'
        if len(encoded)>160 or mode not in (0o644,0o755):raise ValueError('Invalid addon metadata')
        packed=zlib.compress(data,9)
        body+=struct.pack('<IIQQ',len(encoded),mode,len(data),len(packed))+pad(encoded)+pad(packed)
    return b'NGPUADD2'+struct.pack('<IIQ',len(files),0,24+len(body))+body

def decode(data):
    if len(data)<24 or data[:8]!=b'NGPUADD2':raise ValueError('Bad addon magic')
    count,reserved,total=struct.unpack_from('<IIQ',data,8)
    if reserved or total!=len(data) or count>4096:raise ValueError('Bad addon header')
    at=24;files=[]
    for _ in range(count):
        n,mode,size,packed=struct.unpack_from('<IIQQ',data,at);at+=24
        if not 2<=n<=160 or at+n>len(data):raise ValueError('Bad addon name')
        name=data[at:at+n]
        if name[-1] or b'\0' in name[:-1]:raise ValueError('Bad addon terminator')
        at+=(n+7)&~7
        if packed>len(data)-at:raise ValueError('Truncated addon')
        stream=zlib.decompressobj();raw=stream.decompress(data[at:at+packed])+stream.flush()
        if not stream.eof or stream.unused_data or len(raw)!=size:raise ValueError('Invalid addon stream')
        at+=(packed+7)&~7;files.append((name[:-1].decode(),mode,raw))
    if at!=len(data):raise ValueError('Trailing addon bytes')
    return files

def addon_adt(data,core):
    """Change exactly one property, preserving the qualified tree's raw nodes."""
    sys.path.insert(0,str(core/'scripts'))
    from unixnu.devicetree import node as encode_node
    nodes={}
    def walk(at,parent,depth):
        if depth>64 or at+8>len(data):raise ValueError('Invalid ADT node')
        start=at;props,children=struct.unpack_from('<II',data,at);at+=8
        if props>4096 or children>4096:raise ValueError('Unbounded ADT')
        values=[]
        for _ in range(props):
            if at+36>len(data):raise ValueError('Truncated ADT property')
            name,rawsize=struct.unpack_from('<32sI',data,at);at+=36
            if rawsize&0x80000000:raise ValueError('Unsupported ADT property flags')
            if rawsize>len(data)-at:raise ValueError('Truncated ADT data')
            name=name.split(b'\0',1)[0].decode();value=data[at:at+rawsize];at+=(rawsize+3)&~3
            if name in dict(values):raise ValueError('Duplicate ADT property')
            values.append((name,value))
        names=[v for k,v in values if k=='name']
        if len(names)!=1 or not names[0].endswith(b'\0'):raise ValueError('Missing ADT node name')
        name=names[0][:-1].decode();path=parent+'/'+name;child_raw=[]
        for _ in range(children):
            child_start=at;at=walk(at,path,depth+1);child_raw.append(data[child_start:at])
        if path in nodes:raise ValueError('Duplicate ADT path')
        nodes[path]=(start,at,values,child_raw,name)
        return at
    if walk(0,'',0)!=len(data):raise ValueError('Trailing ADT bytes')
    candidates=[v for k,v in nodes.items() if k.endswith('/chosen/memory-map')]
    if len(candidates)!=1:raise ValueError('Ambiguous chosen memory map')
    start,end,values,children,name=candidates[0]
    if 'GpuAddon' in dict(values):raise ValueError('Existing addon must be inspected first')
    if not values or values[0]!=('name',name.encode()+b'\0'):raise ValueError('Unexpected ADT property order')
    if encode_node(name,values[1:],children)!=data[start:end]:raise ValueError('ADT encoder would alter original data')
    replacement=encode_node(name,[*values[1:],('GpuAddon',bytes(16))],children)
    result=data[:start]+replacement+data[end:]
    # Only the property's count word and its 52 new bytes differ.
    insertion=start+8+sum(36+((len(v)+3)&~3) for _,v in values)
    expected=bytearray(data[:insertion]+struct.pack('<32sI',b'GpuAddon',16)+bytes(16)+data[insertion:])
    struct.pack_into('<I',expected,start,len(values)+1)
    if result!=expected:raise ValueError('Unrelated ADT change')
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('artifact',type=Path);p.add_argument('download_receipt',type=Path)
    p.add_argument('baseline_trust',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--source-commit',required=True);p.add_argument('--core',type=Path,required=True)
    p.add_argument('--adt',type=Path,required=True)
    p.add_argument('--kernel',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    record=json.loads(a.download_receipt.read_text())
    if record['conclusion']!='success' or record['commit']!=a.source_commit:raise ValueError('Wrong successful frontend run')
    hashes={row['path'].replace('\\','/'):row['sha256'] for row in record['files']}
    artifact=a.artifact.resolve()
    def read(path):
        if path.is_symlink() or not path.is_file():raise ValueError('Require a regular artifact file')
        data=path.read_bytes()
        if hashlib.sha256(data).hexdigest()!=hashes[path.relative_to(artifact).as_posix()]:raise ValueError('Artifact hash drift')
        return data
    upstream=artifact/'viva-gpu';out=upstream/'ios/out';bundle=out/'VivaMTLDriver.bundle'
    receipt=json.loads(read(upstream/'g52-source-receipt.json'))
    if receipt['mock_selectable'] is not False or receipt['transport']!='NativeMali type-0 connection, selectors 1-10/12/13':
        raise ValueError('Not the NativeMali frontend')
    probe=read(out/'tests/native-metal-probe');plugin=read(bundle/'VivaMTLDriver')
    program_signature=code_directory(probe,True);plugin_signature=code_directory(plugin,False)
    if 'CDHash='+plugin_signature['cdhash'] not in read(artifact/'signature.txt').decode():raise ValueError('Bundle signing receipt mismatch')
    files=[]
    for path in sorted(bundle.rglob('*')):
        if path.is_dir():continue
        files.append(('addons/VivaMTLDriver.bundle/'+path.relative_to(bundle).as_posix(),0o755 if path.name=='VivaMTLDriver' else 0o644,read(path)))
    # Match the existing diagnostic write tracer by basename, without altering
    # the signed binary, its identifier, service class, or client permissions.
    # This readonly bind view pins the APFS source before phase-2 covers /var
    # with tmpfs. Execute the trusted file, not the later tmpfs template copy.
    files.append(('addons/mali-user-probe',0o755,probe))
    files.append(('addons/native-metal-probe.metallib',0o644,read(out/'tests/native-metal-probe.metallib')))
    cache=module('owner_metal_cache',ROOT/'scripts/prepare-mali-metal-launchd.py')
    packed_cache,raw_cache,_=cache.verified_cache((ROOT/'patches/xnu/bbk-a2-splashboard-prewarm.patch').read_text())
    if packed_cache not in a.kernel.read_bytes():
        raise ValueError('Kernel does not contain the merged owner Metal launchd job')
    job=cache.job()
    files.append(('daemons/org.unixnu.bbk.mali-metal.plist',0o644,plistlib.dumps(job,sort_keys=True)))
    upstream_packer=module('ngpu_packer',a.core/'scripts/make-gpu-addon.py')
    addon=encode(files,upstream_packer.pad)
    if decode(addon)!=files:raise ValueError('Addon roundtrip mismatch')
    seed=module('seed_trust',ROOT/'scripts/prepare-data-seed.py')
    baseline=a.baseline_trust.read_bytes();seed._validate_trust_bundle(baseline)
    count=struct.unpack_from('<I',baseline)[0]
    if count!=4:raise ValueError('Require the qualified four-module baseline')
    offsets=[*struct.unpack_from('<4I',baseline,4),len(baseline)]
    modules=[baseline[start:end] for start,end in zip(offsets,offsets[1:])]
    cdhashes=sorted({program_signature['cdhash'],plugin_signature['cdhash']})
    if len(cdhashes)!=2:raise ValueError('Expected two separate signed owner images')
    identity=uuid.uuid5(uuid.NAMESPACE_URL,'org.unixnu.bbk.mali-metal:'+','.join(cdhashes)).bytes
    owner=struct.pack('<I16sI',2,identity,2)+b''.join(bytes.fromhex(h)+bytes([2,0,5,0]) for h in cdhashes)
    trusted=seed._pack_trust_modules([owner,*modules])
    checked=[*struct.unpack_from('<5I',trusted,4),len(trusted)]
    if [trusted[s:e] for s,e in zip(checked[1:],checked[2:])]!=modules:raise ValueError('Original trust module changed')
    tree=addon_adt(a.adt.read_bytes(),a.core.resolve())
    for name,data in [('mali-metal.gpuaddon',addon),('kernel.macho.trustcache',trusted),('kernel.macho.adt',tree)]:
        with (a.output/name).open('xb') as f:f.write(data)
    result=dict(frontend_run=record['run'],source_commit=a.source_commit,hardware_tested=False,
        automatic_system_accelerator=False,original_trust_modules_preserved=4,external_trust_modules=5,
        probe_signature=program_signature,plugin_signature=plugin_signature,
        addon_bytes=len(addon),addon_sha256=hashlib.sha256(addon).hexdigest(),
        trust_sha256=hashlib.sha256(trusted).hexdigest(),trust_bytes=len(trusted),
        adt_before_sha256=hashlib.sha256(a.adt.read_bytes()).hexdigest(),
        adt_sha256=hashlib.sha256(tree).hexdigest(),adt_added_property='/chosen/memory-map/GpuAddon',
        owner_launchd_cache_sha256=hashlib.sha256(raw_cache).hexdigest(),
        files=[dict(path=n,mode=oct(m),bytes=len(b),sha256=hashlib.sha256(b).hexdigest()) for n,m,b in files])
    (a.output/'addon.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
