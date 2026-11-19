#!/usr/bin/env python3
"""Run real NativeMali session logic with host-only power/register/allocation doubles."""
import os, shutil, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=root/"drivers/SprdPlatform/mali"
compiler=shutil.which("clang++")
if not compiler: raise SystemExit("clang++ is required")
with tempfile.TemporaryDirectory(prefix="mali-session-") as temporary:
    work=Path(temporary)
    for p in source.glob("*"):
        if p.is_file():shutil.copyfile(p,work/p.name)
    (work/"NativeMaliPower.h").write_text(
        '#pragma once\n#include <IOKit/IOTypes.h>\n'
        'class NativeMaliPower { public: bool prepared()const{return true;}'
        'IOReturn gpuReadable()const{return kIOReturnSuccess;} };\n')
    (work/"IOKit").mkdir()
    shutil.copyfile(root/"scripts/mali-host/shim/IOKit/IOTypes.h",work/"IOKit/IOTypes.h")
    (work/"IOKit/IOLib.h").write_text(
        '#pragma once\n#include <cstdlib>\n'
        '#define IONewData(T,n) static_cast<T *>(std::calloc((n),sizeof(T)))\n'
        '#define IODeleteData(p,T,n) std::free(p)\n')
    modules=["Vm","Mmu","Gpu","GpuCore","Fault","Job","JobSlot","Memory","BoTable","AbiValidate","UserClient"]
    command=[compiler,"-std=c++17","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
        "-fno-omit-frame-pointer","-DNATIVE_MALI_M3_WRITES=1","-DNATIVE_MALI_M4_WRITES=1",
        "-DNATIVE_MALI_M6_WRITES=1","-I",str(work),
        str(root/"scripts/tests/test-sprd-mali-session.cpp"),
        *[str(work/("NativeMali"+m+".cpp")) for m in modules],"-o",str(work/"test")]
    subprocess.run(command,check=True)
    subprocess.run([str(work/"test")],check=True)
