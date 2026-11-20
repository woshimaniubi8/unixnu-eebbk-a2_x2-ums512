#!/usr/bin/env python3
"""Compile the actual Data scratch patch with allocation/lifetime fault tests."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
patch=root/'patches/xnu/bbk-a2-owner-data-scratch.patch'
original='''#if defined(XNU_OWNER_DATA_MOUNT) && XNU_OWNER_DATA_MOUNT
\tint data_error = native_owner_data_prepare(p, (user_addr_t)scratch_addr);
\tif (data_error) panic("offline scratch Data mount failed: %d", data_error);
#endif
'''
header=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define XNU_OWNER_DATA_MOUNT 1
#define KERN_SUCCESS 0
#define VM_FLAGS_ANYWHERE 1
#define VM_KERN_MEMORY_NONE 0
typedef uintptr_t mach_vm_offset_t;
#ifndef __APPLE__
typedef uintptr_t user_addr_t;
#endif
typedef int kern_return_t;
static unsigned allocations,frees;
static int allocation_error,data_error_input;
static jmp_buf panic_target;
static unsigned char *active;
static unsigned char original[16384],saved[16384];
static void panic(const char *text,...) { (void)text;longjmp(panic_target,1); }
static int copyin(user_addr_t from,void *to,size_t n) {memcpy(to,(void *)from,n);return 0;}
static kern_return_t mach_vm_allocate_kernel(void *map,mach_vm_offset_t *out,size_t bytes,int flags,int tag) {
    (void)map;(void)flags;(void)tag;
    if(allocation_error)return allocation_error;
    active=calloc(1,bytes);assert(active);*out=(uintptr_t)active;++allocations;return 0;
}
static kern_return_t mach_vm_deallocate(void *map,mach_vm_offset_t at,size_t bytes) {
    (void)map;(void)bytes;assert((void *)at==active);free(active);active=NULL;++frees;return 0;
}
static int native_owner_data_prepare(void *p,user_addr_t scratch) {
    (void)p;
    memcpy((void *)(scratch+0x500),"/private/var/ngpu/addons",24);
    memcpy((void *)(scratch+0xc00),"/private/var/ngpu/daemons",25);
    return data_error_input;
}
static void boot(size_t map_page_size) {
    void *p=NULL,*map=NULL;
    uintptr_t scratch_addr=(uintptr_t)original;
    user_addr_t init_envp=scratch_addr+0x500;
'''
footer=r'''
}
int main(void) {
    uint64_t pointer=(uintptr_t)original+0x300;
    memcpy(original+0x500,&pointer,sizeof(pointer));
    memcpy(saved,original,sizeof(saved));
    native_owner_data_prepare(NULL,(uintptr_t)original);
    uint64_t clobbered=0;memcpy(&clobbered,original+0x500,8);
    assert(clobbered==UINT64_C(0x657461766972702f)); /* observed '/private' */
    for(unsigned i=0;i<4;i++) {
        memcpy(original,saved,sizeof(saved));
        allocation_error=i==2?5:0;data_error_input=i==3?5:0;
        int panicked=setjmp(panic_target);
        if(!panicked)boot(i==0?4096:16384);
        assert(panicked==(i>=2));
        assert(!memcmp(original,saved,sizeof(saved)));
        assert(allocations==frees && !active);
    }
    puts("PASS: original /private envp corruption reproduced; actual patched code isolates 4K/16K scratch and releases on success/error.");
}
'''
with tempfile.TemporaryDirectory(prefix='owner-scratch-') as d:
    out=Path(d);source=out/'bsd/kern/kern_exec.c';source.parent.mkdir(parents=True)
    before=header+original+footer;source.write_text(before)
    for args in [['--check'],[]]:
        subprocess.run(['git','apply',*args,str(patch)],cwd=out,check=True,capture_output=True)
    after=source.read_text()
    compiler=shutil.which('clang')
    if not compiler:raise SystemExit('clang is required')
    subprocess.run([compiler,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(source),'-o',str(out/'check')],check=True)
    result=subprocess.run([str(out/'check')],capture_output=True,text=True,check=True)
    print(result.stdout.splitlines()[-1])
    subprocess.run(['git','apply','--reverse',str(patch)],cwd=out,check=True,capture_output=True)
    assert source.read_text()==before
print('PASS: boot scratch patch applies and reverses exactly.')
