#!/usr/bin/env python3
"""Check the cached owner job is gone and all 561 system services survive."""
import copy
import importlib.util
from pathlib import Path

root=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('metal_cache',root/'scripts/prepare-mali-metal-launchd.py')
m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
patch=root/'patches/xnu/bbk-a2-splashboard-prewarm.patch'
m.prepare(patch,remove_job=True)
text=patch.read_text()
packed,raw,config=m.verified_cache(text,False)
previous=m.validate(config,False)
assert len(previous['LaunchDaemons'])==561 and len(config['LaunchDaemons'])==561
# The kernel stage spawns the owner probe before launchd; a cached job for the
# tmpfs copy only adds a MACF EPERM at every boot.
assert m.JOB_PATH not in config['LaunchDaemons']
for path in m.STATS_JOBS:
    assert config['LaunchDaemons'][path]['EnvironmentVariables'][m.STATS_ENV]=='1'
assert config['LaunchDaemons'][m.STATS_JOBS[1]]['EnvironmentVariables']['CA_ASSERT_MAIN_THREAD_TRANSACTIONS']=='1'
tampered=copy.deepcopy(config)
tampered['LaunchDaemons'][next(iter(previous['LaunchDaemons']))]['RunAtLoad']='modified'
try:m.validate(tampered,False)
except ValueError:pass
else:raise AssertionError('A system service policy change was accepted')
tampered=copy.deepcopy(config)
tampered['LaunchDaemons'][m.STATS_JOBS[0]]['EnvironmentVariables'][m.STATS_ENV]='0'
try:m.validate(tampered,False)
except ValueError:pass
else:raise AssertionError('An unqualified statistics setting was accepted')
print('PASS: owner job absent; only two UI statistics environment keys added; all other system configuration intact.')

