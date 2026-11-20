#!/usr/bin/env python3
"""Merge one owner GPU qualification job into the existing xpc boot cache.

The original boot loader uses this merged cache, not a directory scan of newly
overlaid daemon plists. Preserve every previous service and all other keys.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import plistlib
import re
import subprocess
import zlib

ROOT=Path(__file__).resolve().parents[1]
LABEL='org.unixnu.bbk.mali-metal'
JOB_PATH='/System/Library/LaunchDaemons/'+LABEL+'.plist'
BASELINE_CANONICAL_SHA='4e8ccab53acfae99eef9231197cea63e8d27d48bebd643487edafa1b9f59bdc9'

def array(text,name):
    match=re.search(r'(?s)\+    static const uint8_t '+name+r'\[(?:32)?\] = \{(.*?)\n\+    \};',text)
    if not match:raise ValueError('Missing cache array '+name)
    return bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',match[1]))

def canonical(value):
    return hashlib.sha256(plistlib.dumps(value,fmt=plistlib.FMT_BINARY,sort_keys=True)).hexdigest()

def job():
    return plistlib.loads((ROOT/'drivers/SprdPlatform/userspace/mali-metal-job.plist').read_bytes())

def validate(config,require_job):
    old=copy.deepcopy(config)
    entry=old['LaunchDaemons'].pop(JOB_PATH,None)
    if require_job and entry!=job():raise ValueError('Owner Metal job differs or is absent')
    if entry is not None and entry!=job():raise ValueError('Conflicting owner label')
    if canonical(old)!=BASELINE_CANONICAL_SHA:raise ValueError('Unrelated system cache configuration changed')
    return old

def verified_cache(text):
    packed=array(text,'packed');raw=zlib.decompress(packed)
    if hashlib.sha256(raw).digest()!=array(text,'modified_sha'):raise ValueError('Cache digest mismatch')
    size=int(re.search(r'configuration_size = (\d+);',text)[1])
    if size!=len(raw):raise ValueError('Cache size mismatch')
    config=plistlib.loads(raw);validate(config,True)
    return packed,raw,config

def c_array(name,data,fixed=False):
    lines=['+    static const uint8_t '+name+('[32]' if fixed else '[]')+' = {']
    for i in range(0,len(data),16):lines.append('+\t'+', '.join('0x%02x'%b for b in data[i:i+16])+',')
    return '\n'.join([*lines,'+    };'])

def prepare(path):
    before=path.read_text();raw=zlib.decompress(array(before,'packed'))
    if hashlib.sha256(raw).digest()!=array(before,'modified_sha'):raise ValueError('Previous cache pin mismatch')
    config=plistlib.loads(raw);baseline=validate(config,False)
    if JOB_PATH in config['LaunchDaemons']:
        verified_cache(before);return dict(already_configured=True,sha256=hashlib.sha256(raw).hexdigest())
    config['LaunchDaemons'][JOB_PATH]=job()
    validate(config,True)
    modified=plistlib.dumps(config,fmt=plistlib.FMT_BINARY,sort_keys=True)
    after=before
    for name,data,fixed in [('packed',zlib.compress(modified,9),False),('modified_sha',hashlib.sha256(modified).digest(),True)]:
        pattern=r'(?s)\+    static const uint8_t '+name+r'\[(?:32)?\] = \{.*?\n\+    \};'
        after,count=re.subn(pattern,lambda _:c_array(name,data,fixed),after)
        if count!=1:raise ValueError('Ambiguous cache replacement')
    after,count=re.subn(r'configuration_size = '+str(len(raw))+r';','configuration_size = '+str(len(modified))+';',after)
    if count!=1:raise ValueError('Ambiguous cache extent')
    after=after.replace('+ * cache. Only the original SplashBoard job gains RunAtLoad; executable trust,\n+ * user clients, snapshot results and watchdog policy remain unchanged. */',
        '+ * cache. The existing SplashBoard warm-up and one owned Mali Metal test are\n+ * declared here; system jobs, executable trust and watchdog policy stay intact. */')
    delta=after.count('\n')-before.count('\n')
    after,count=re.subn(r'@@ -7780,6 \+7780,17907 @@','@@ -7780,6 +7780,'+str(17907+delta)+' @@',after)
    if count!=1:raise ValueError('Unexpected first cache patch hunk')
    after,count=re.subn(r'@@ -7816,6 \+25717,8 @@','@@ -7816,6 +'+str(25717+delta)+',8 @@',after)
    if count!=1:raise ValueError('Unexpected second cache patch hunk')
    verified_cache(after)
    if array(after,'original_sha')!=array(before,'original_sha'):raise ValueError('Original OS cache pin changed')
    path.write_text(after,newline='\n')
    subprocess.run(['git','apply','--numstat',str(path.resolve())],cwd=ROOT,check=True,capture_output=True)
    return dict(already_configured=False,original_system_jobs_preserved=len(baseline['LaunchDaemons']),
        job_path=JOB_PATH,before_sha256=hashlib.sha256(raw).hexdigest(),after_sha256=hashlib.sha256(modified).hexdigest(),
        before_bytes=len(raw),after_bytes=len(modified),source_patch_sha256=hashlib.sha256(path.read_bytes()).hexdigest())

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patch',type=Path,default=ROOT/'patches/xnu/bbk-a2-splashboard-prewarm.patch')
    p.add_argument('--report',type=Path,required=True)
    a=p.parse_args();record=prepare(a.patch);a.report.write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
