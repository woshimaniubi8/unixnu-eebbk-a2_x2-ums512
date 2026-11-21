#!/usr/bin/env python3
"""Compile the shipping userspace backend with sanitizer transport fault tests."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('viva',type=Path)
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
src=root/'drivers/SprdPlatform/userspace'
viva=a.viva.resolve()
with tempfile.TemporaryDirectory(prefix='native-mali-backend-') as tmp:
    out=Path(tmp)
    common=['-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
            '-I'+str(src),'-I'+str(viva/'vgpu')]
    subprocess.run([shutil.which('clang') or 'clang','-std=c11','-D_POSIX_C_SOURCE=200809L','-pthread',*common,
        str(src/'vgpu_native_mali.c'),str(root/'scripts/tests/test-mali-native-backend.c'),'-o',str(out/'backend')],check=True)
    subprocess.run([str(out/'backend')],check=True)
    subprocess.run([shutil.which('clang++') or 'clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-enum-compare',
        '-I'+str(src),'-I'+str(root/'drivers/SprdPlatform/mali'),str(root/'scripts/tests/test-mali-wire.cpp'),
        '-o',str(out/'wire')],check=True)
    subprocess.run([str(out/'wire')],check=True)
    subprocess.run([shutil.which('clang') or 'clang','-std=c11','-D_POSIX_C_SOURCE=200809L','-pthread',*common,
        str(root/'scripts/tests/test-mali-surface-fd.c'),'-o',str(out/'surface-fd')],check=True)
    subprocess.run([str(out/'surface-fd')],check=True)
print('PASS: NativeMali record sizes and offsets agree with kernel wire ABI.')
