#!/usr/bin/env python3
"""Check the exact cached owner job and preserve all 561 prior services."""
import copy
import importlib.util
from pathlib import Path

root=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('metal_cache',root/'scripts/prepare-mali-metal-launchd.py')
m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
text=(root/'patches/xnu/bbk-a2-splashboard-prewarm.patch').read_text()
packed,raw,config=m.verified_cache(text)
previous=m.validate(config,True)
assert len(previous['LaunchDaemons'])==561 and len(config['LaunchDaemons'])==562
entry=config['LaunchDaemons'][m.JOB_PATH]
assert entry['RunAtLoad'] is True and entry['LaunchOnlyOnce'] is True and entry['KeepAlive'] is False
assert entry['ProgramArguments'][0]=='/private/var/ngpu/bin/mali-user-probe'
# Stock system-domain daemons inherit root; UserName routes through the
# per-user launch configuration and xpcproxy's account/group setup.
assert 'UserName' not in entry and 'GroupName' not in entry
tampered=copy.deepcopy(config)
tampered['LaunchDaemons'][next(iter(previous['LaunchDaemons']))]['RunAtLoad']='modified'
try:m.validate(tampered,True)
except ValueError:pass
else:raise AssertionError('A system service policy change was accepted')
print('PASS: the real xpc cache contains one owner Metal job; all 561 existing services and other keys are intact.')
