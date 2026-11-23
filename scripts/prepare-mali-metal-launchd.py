#!/usr/bin/env python3
"""Merge the owner GPU job and two scoped Viva statistics settings into xpc cache.

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
STATS_ENV='VMG_NO_SHM_STATS'
STATS_JOBS=('/System/Library/LaunchDaemons/com.apple.backboardd.plist',
            '/System/Library/LaunchDaemons/com.apple.SpringBoard.plist')

def array(text,name):
    match=re.search(r'(?s)\+    static const uint8_t '+name+r'\[(?:32)?\] = \{(.*?)\n\+    \};',text)
    if not match:raise ValueError('Missing cache array '+name)
    return bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',match[1]))

def canonical(value):
    return hashlib.sha256(plistlib.dumps(value,fmt=plistlib.FMT_BINARY,sort_keys=True)).hexdigest()

def compress_cache(data):
    # Keep the standard zlib format; a smaller encoder hash workspace is
    # measurably better for this binary plist than zlib's default workspace.
    encoder=zlib.compressobj(9,zlib.DEFLATED,zlib.MAX_WBITS,7)
    packed=encoder.compress(data)+encoder.flush()
    if zlib.decompress(packed)!=data:raise ValueError('Cache compression roundtrip')
    return packed

def job():
    return plistlib.loads((ROOT/'drivers/SprdPlatform/userspace/mali-metal-job.plist').read_bytes())

def validate(config,require_job):
    old=copy.deepcopy(config)
    entry=old['LaunchDaemons'].pop(JOB_PATH,None)
    if require_job and entry!=job():raise ValueError('Owner Metal job differs or is absent')
    if require_job and entry is not None and entry!=job():raise ValueError('Conflicting owner label')
    for path in STATS_JOBS:
        target=old['LaunchDaemons'][path]
        env=target.get('EnvironmentVariables',{})
        value=env.pop(STATS_ENV,None)
        if value not in (None,'1') or (require_job and value!='1'):
            raise ValueError('Unexpected Viva statistics environment policy')
        if 'EnvironmentVariables' in target and not env:
            del target['EnvironmentVariables']
    if canonical(old)!=BASELINE_CANONICAL_SHA:raise ValueError('Unrelated system cache configuration changed')
    return old


def recount_patch(text):
    lines=text.splitlines(keepends=True)
    delta=0
    for i,line in enumerate(lines):
        if line.startswith('--- a/'):
            delta=0
        match=re.match(r'@@ -(\d+),\d+ \+\d+,\d+ @@(.*)',line)
        if not match:continue
        end=i+1
        while end<len(lines) and not lines[end].startswith(('@@ ','--- a/','diff --git ')):
            end+=1
        body=lines[i+1:end]
        before=sum(value.startswith((' ','-')) for value in body)
        after=sum(value.startswith((' ','+')) for value in body)
        start=int(match[1])
        lines[i]=f'@@ -{start},{before} +{start+delta},{after} @@{match[2]}\n'
        delta+=after-before
    return ''.join(lines)

def verified_cache(text,require_job=True):
    packed=array(text,'packed');raw=zlib.decompress(packed)
    if hashlib.sha256(raw).digest()!=array(text,'modified_sha'):raise ValueError('Cache digest mismatch')
    size=int(re.search(r'configuration_size = (\d+);',text)[1])
    if size!=len(raw):raise ValueError('Cache size mismatch')
    config=plistlib.loads(raw);validate(config,require_job)
    return packed,raw,config

def c_array(name,data,fixed=False):
    lines=['+    static const uint8_t '+name+('[32]' if fixed else '[]')+' = {']
    for i in range(0,len(data),16):lines.append('+\t'+', '.join('0x%02x'%b for b in data[i:i+16])+',')
    return '\n'.join([*lines,'+    };'])

def prepare(path, remove_job=False):
    before=path.read_text();raw=zlib.decompress(array(before,'packed'))
    if hashlib.sha256(raw).digest()!=array(before,'modified_sha'):raise ValueError('Previous cache pin mismatch')
    config=plistlib.loads(raw);baseline=validate(config,False)
    desired=copy.deepcopy(config)
    if remove_job:
        # The kernel stage spawns the owner probe before launchd; a cached job
        # for the same tmpfs path only adds an EPERM from MACF.
        desired['LaunchDaemons'].pop(JOB_PATH,None)
    else:
        desired['LaunchDaemons'][JOB_PATH]=job()
    for target in STATS_JOBS:
        # Viva's own opt-out prevents its optional Manager statistics page
        # from calling umask(0) inside Apple's restricted UI services.
        desired['LaunchDaemons'][target].setdefault('EnvironmentVariables',{})[STATS_ENV]='1'
    if desired==config:
        verified_cache(before,not remove_job);return dict(already_configured=True,sha256=hashlib.sha256(raw).hexdigest())
    config=desired
    validate(config,not remove_job)
    modified=plistlib.dumps(config,fmt=plistlib.FMT_BINARY,sort_keys=True)
    after=before
    for name,data,fixed in [('packed',compress_cache(modified),False),('modified_sha',hashlib.sha256(modified).digest(),True)]:
        pattern=r'(?s)\+    static const uint8_t '+name+r'\[(?:32)?\] = \{.*?\n\+    \};'
        after,count=re.subn(pattern,lambda _:c_array(name,data,fixed),after)
        if count!=1:raise ValueError('Ambiguous cache replacement')
    after,count=re.subn(r'configuration_size = '+str(len(raw))+r';','configuration_size = '+str(len(modified))+';',after)
    if count!=1:raise ValueError('Ambiguous cache extent')
    after=after.replace('+ * cache. Only the original SplashBoard job gains RunAtLoad; executable trust,\n+ * user clients, snapshot results and watchdog policy remain unchanged. */',
        '+ * cache. The existing SplashBoard warm-up and owned Mali Metal test remain;\n+ * two UI jobs opt out of optional Viva shared statistics. Trust and watchdog policy stay intact. */')
    after=after.replace('+ * cache. The existing SplashBoard warm-up and one owned Mali Metal test are\n+ * declared here; system jobs, executable trust and watchdog policy stay intact. */',
        '+ * cache. The existing SplashBoard warm-up and owned Mali Metal test remain;\n+ * two UI jobs opt out of optional Viva shared statistics. Trust and watchdog policy stay intact. */')
    after=recount_patch(after)
    verified_cache(after,not remove_job)
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
    p.add_argument('--remove-job',action='store_true',help='drop the cached owner Metal job; the kernel stage spawns the probe')
    a=p.parse_args();record=prepare(a.patch,a.remove_job);a.report.write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))


