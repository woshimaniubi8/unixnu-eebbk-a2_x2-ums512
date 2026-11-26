#!/usr/bin/env python3
"""Check the cached Metal probe job and preserve all 561 prior services."""
import copy
import importlib.util
from pathlib import Path

root=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('metal_cache',root/'scripts/prepare-mali-metal-launchd.py')
m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
patch=root/'patches/xnu/bbk-a2-splashboard-prewarm.patch'
m.prepare(patch)
text=patch.read_text()
packed,raw,config=m.verified_cache(text)
previous=m.validate(config,True)
assert len(previous['LaunchDaemons'])==560 and len(config['LaunchDaemons'])==561
assert '/System/Library/LaunchDaemons/com.apple.splashboardd.plist' not in config['LaunchDaemons']
entry=config['LaunchDaemons'][m.JOB_PATH]
assert entry['RunAtLoad'] is True and entry['LaunchOnlyOnce'] is True and entry['KeepAlive'] is False
assert entry['ProgramArguments']==['/private/var/ngpu/addons/native-metal-probe']
assert entry['LimitLoadToSessionType']=='System'
for path in m.STATS_JOBS:
    assert config['LaunchDaemons'][path]['EnvironmentVariables'][m.STATS_ENV]=='1'
assert config['LaunchDaemons'][m.STATS_JOBS[1]]['EnvironmentVariables']['CA_ASSERT_MAIN_THREAD_TRANSACTIONS']=='1'
tampered=copy.deepcopy(config)
tampered['LaunchDaemons'][next(iter(previous['LaunchDaemons']))]['RunAtLoad']='modified'
try:m.validate(tampered,True)
except ValueError:pass
else:raise AssertionError('A system service policy change was accepted')
tampered=copy.deepcopy(config)
tampered['LaunchDaemons'][m.STATS_JOBS[0]]['EnvironmentVariables'][m.STATS_ENV]='0'
try:m.validate(tampered,True)
except ValueError:pass
else:raise AssertionError('An unqualified statistics setting was accepted')
print('PASS: cached Metal probe job present; only two UI statistics environment keys added; all other system configuration intact.')

