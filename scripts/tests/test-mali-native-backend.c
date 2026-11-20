/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A transport fault injector, not an emulated GPU. The identical backend C
 * file is compiled into the device Metal bundle. Execution is not claimed. */
#include "vgpu.h"
#include "mali_native_wire.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct transport {
    uint64_t next,now,fence;
    int fail,map_fail,wait_error,closed,submit_calls,unmaps;
    unsigned allocs,frees;
    unsigned imports,surface_releases,shared_waits;
    int shared_busy,malformed_surface;
    void *surface_memory;
    struct {uint64_t id,size;void *cpu;int imported;} bos[64];
};
static int invoke(void *context,uint32_t method,const void *in,size_t ni,void *out,size_t no)
{
    struct transport *t=context;(void)ni;(void)no;
    if(t->fail==(int)method)return -EIO;
    switch(method){
    case NM_QUERY:*(struct nm_query *)out=(struct nm_query){2,35,40,0,1u<<27};break;
    case NM_PARAM:{const struct nm_param *p=in;*(struct nm_param *)out=(struct nm_param){p->parameter,0,p->parameter==0?0x7402:0x1000};break;}
    case NM_ALLOC:{
        const struct nm_alloc *a=in;unsigned i=0;while(i<64 && t->bos[i].id)i++;assert(i<64);
        t->bos[i].id=++t->next;t->bos[i].size=a->bytes;t->bos[i].cpu=calloc(1,(size_t)a->bytes);assert(t->bos[i].cpu);
        *(struct nm_handle *)out=(struct nm_handle){t->bos[i].id};t->allocs++;break;
    }
    case NM_IMPORT_USER:{
        const struct nm_import_user *a=in;assert(!a->flags && !a->reserved);
        unsigned i=0;while(i<64 && t->bos[i].id)i++;assert(i<64);
        t->bos[i].id=++t->next;t->bos[i].size=a->bytes;
        t->bos[i].cpu=(void *)(uintptr_t)a->address;t->bos[i].imported=1;
        *(struct nm_handle *)out=(struct nm_handle){t->bos[i].id};t->imports++;break;
    }
    case NM_MAP:{const struct nm_map *a=in;assert(a->permissions==3 || a->permissions==7);*(struct nm_va *)out=(struct nm_va){a->va};break;}
    case NM_FREE:{
        uint64_t h=((const struct nm_handle *)in)->handle;unsigned i=0;while(i<64 && t->bos[i].id!=h)i++;assert(i<64);
        if(!t->bos[i].imported)free(t->bos[i].cpu);
        memset(&t->bos[i],0,sizeof(t->bos[i]));t->frees++;break;
    }
    case NM_CPU_MAP:{
        uint64_t h=((const struct nm_handle *)in)->handle;unsigned i=0;while(i<64 && t->bos[i].id!=h)i++;assert(i<64);
        *(struct nm_cpu_map *)out=(struct nm_cpu_map){i+1,0,t->bos[i].size};break;
    }
    case NM_CPU_UNMAP:break;
    case NM_SUBMIT:t->submit_calls++;*(struct nm_fence_wire *)out=(struct nm_fence_wire){++t->fence};break;
    case NM_WAIT:t->now+=1000000000;return t->wait_error;
    case NM_CACHE_SYNC:break;
    case NM_WAIT_SHARED:
        t->shared_waits++;t->now+=1000;
        if(t->shared_busy){t->shared_busy--;return -EBUSY;}break;
    default:assert(0);
    }return 0;
}
static int map(void *context,uint32_t token,void **out,size_t *size)
{
    struct transport *t=context;if(t->map_fail)return -ENOMEM;
    assert(token && token<=64 && t->bos[token-1].id);*out=t->bos[token-1].cpu;*size=t->bos[token-1].size;return 0;
}
static int unmap(void *context,uint32_t token,void *cpu)
{struct transport *t=context;assert(t->bos[token-1].cpu==cpu);t->unmaps++;return 0;}
static void close_connection(void *context)
{
    struct transport *t=context;t->closed++;
    for(unsigned i=0;i<64;i++)if(t->bos[i].id){if(!t->bos[i].imported)free(t->bos[i].cpu);t->bos[i].id=0;t->frees++;}
}
static uint64_t now(void *context){return ((struct transport *)context)->now;}
static int surface(void *context,uint32_t id,uint32_t plane,struct nm_surface_view *out)
{
    struct transport *t=context;if(id!=7 || plane)return -ENOENT;
    *out=(struct nm_surface_view){.address=(uintptr_t)t->surface_memory+t->malformed_surface,
        .bytes=32768,.in_page=4096,.plane_bytes=28672,.bytes_per_row=256,
        .pixel_format=0x52474241,.width=64,.height=64,.retained=t};
    return 0;
}
static void release_surface(void *value){((struct transport *)value)->surface_releases++;}
int nm_iokit_open(void **out){(void)out;return -ENOTSUP;}
#define IO(sel,a) vgpu_backend_iokit.ioctl(c,VGPU_SEL_##sel,&(a),sizeof(a))
int main(void)
{
    const struct nm_transport ops={invoke,map,unmap,close_connection,now,16384,NULL,NULL};
    struct transport t={.next=UINT64_C(1)<<32};void *c=NULL;assert(nm_backend_connect(&ops,&t,&c)==0 && c);
    struct vgpu_version v={0};assert(IO(GET_VERSION,v)==0 && v.gpu_id==0x74021000 && v.minor==0);
    struct vgpu_create_bo a={.size=4096,.flags=VGPU_BO_NOEXEC};assert(IO(CREATE_BO,a)==0);
    struct vgpu_create_bo b={.size=16384};assert(IO(CREATE_BO,b)==0 && a.handle!=b.handle && b.offset-a.offset==16384);
    assert(t.allocs==2);
    struct vgpu_create_bo heap={.size=4096,.flags=VGPU_BO_HEAP};assert(IO(CREATE_BO,heap)==-EINVAL && t.allocs==2);
    struct vgpu_create_bo invalid={.size=4096,.flags=0x80000000};assert(IO(CREATE_BO,invalid)==-EINVAL);
    void *p=vgpu_backend_iokit.mmap(c,a.handle,4096);assert(p);memset(p,0x5a,4096);
    struct vgpu_gem_close drop={a.handle,0};assert(IO(GEM_CLOSE,drop)==-EBUSY);
    assert(vgpu_backend_iokit.munmap(c,p,4096)==0);
    t.map_fail=1;assert(!vgpu_backend_iokit.mmap(c,a.handle,4096) && errno==ENOMEM);t.map_fail=0;
    struct vgpu_syncobj_create s={.flags=0};assert(IO(SYNCOBJ_CREATE,s)==0);
    uint32_t id=s.handle;
    struct vgpu_syncobj_wait w={(uintptr_t)&id,0,1,VGPU_SYNCOBJ_WAIT_FOR_SUBMIT,0,0,0};
    assert(IO(SYNCOBJ_WAIT,w)==-ETIMEDOUT);
    uint32_t handles[]={a.handle,b.handle};
    struct vgpu_submit submit={.jc=a.offset,.bo_handles=(uintptr_t)handles,.bo_handle_count=2,.out_sync=id};
    assert(IO(SUBMIT,submit)==0 && t.submit_calls==1);
    w.timeout_nsec=INT64_MAX;assert(IO(SYNCOBJ_WAIT,w)==0 && w.first_signaled==0);
    t.wait_error=-EIO;assert(IO(SUBMIT,submit)==-EIO);
    assert(IO(SYNCOBJ_WAIT,w)==-EIO);
    struct vgpu_wait_bo wb={a.handle,0,INT64_MAX};assert(IO(WAIT_BO,wb)==-EIO);
    t.wait_error=0;assert(IO(SUBMIT,submit)==-EIO && t.submit_calls==2); /* sticky device failure */
    assert(IO(GEM_CLOSE,drop)==-EIO);
    vgpu_backend_iokit.close(c);assert(t.closed==1 && t.allocs==t.frees && t.unmaps==1);
    memset(&t,0,sizeof(t));assert(nm_backend_connect(&ops,&t,&c)==0);
    a=(struct vgpu_create_bo){.size=4096};t.fail=NM_MAP;
    assert(IO(CREATE_BO,a)==-EIO && t.allocs==1 && t.frees==1);t.fail=0;
    assert(IO(CREATE_BO,a)==0);handles[0]=a.handle;submit=(struct vgpu_submit){.jc=a.offset,.bo_handles=(uintptr_t)handles,.bo_handle_count=1};
    t.wait_error=-ETIMEDOUT;assert(IO(SUBMIT,submit)==-ETIMEDOUT);
    drop.handle=a.handle;assert(IO(GEM_CLOSE,drop)==-ETIMEDOUT && t.frees==1);
    t.wait_error=0;assert(IO(GEM_CLOSE,drop)==0 && t.frees==2);
    vgpu_backend_iokit.close(c);
    memset(&t,0,sizeof(t));t.fail=NM_PARAM;c=(void *)1;
    assert(nm_backend_connect(&ops,&t,&c)==-EIO && !c && t.closed==1);
    struct nm_transport shared_ops=ops;shared_ops.surface=surface;shared_ops.release_surface=release_surface;
    memset(&t,0,sizeof(t));t.surface_memory=aligned_alloc(16384,32768);assert(t.surface_memory);
    memset(t.surface_memory,0x6b,32768);assert(nm_backend_connect(&shared_ops,&t,&c)==0);
    struct vgpu_import_iosurface im={.surface_id=7};
    t.malformed_surface=1;
    assert(IO(IMPORT_IOSURFACE,im)==-EPROTO && t.imports==0 && t.surface_releases==1);
    t.malformed_surface=0;t.fail=NM_MAP;
    assert(IO(IMPORT_IOSURFACE,im)==-EIO && t.imports==1 && t.frees==1 && t.surface_releases==2);
    t.fail=0;assert(IO(IMPORT_IOSURFACE,im)==0 && im.offset%16384==4096 && im.size==28672);
    assert(((uint8_t *)t.surface_memory)[0]==0x6b); // pinning did not zero the surface
    struct vgpu_query_bo_info qi={.handle=im.handle};
    assert(IO(QUERY_BO_INFO,qi)==0 && qi.extra_flags==VGPU_BO_IS_IMPORTED && qi.create_flags==VGPU_BO_NOEXEC);
    wb=(struct vgpu_wait_bo){im.handle,0,INT64_MAX};t.shared_busy=2;
    assert(IO(WAIT_BO,wb)==0 && t.shared_waits==3);
    t.fail=NM_WAIT_SHARED;assert(IO(WAIT_BO,wb)==-EIO);t.fail=0;
    t.shared_busy=1;wb.timeout_ns=0;assert(IO(WAIT_BO,wb)==-ETIMEDOUT);
    p=vgpu_backend_iokit.mmap(c,im.handle,32768);assert(p==t.surface_memory);
    ((uint8_t *)p)[4096]=0xa7;assert(((uint8_t *)t.surface_memory)[4096]==0xa7);
    drop=(struct vgpu_gem_close){im.handle,0};assert(IO(GEM_CLOSE,drop)==-EBUSY);
    assert(vgpu_backend_iokit.munmap(c,p,32768)==0 && IO(GEM_CLOSE,drop)==0);
    assert(t.surface_releases==3 && t.imports==t.frees);
    im=(struct vgpu_import_iosurface){.surface_id=7};assert(IO(IMPORT_IOSURFACE,im)==0);
    vgpu_backend_iokit.close(c);assert(t.surface_releases==4 && t.imports==t.frees);
    free(t.surface_memory);
    puts("PASS: real adapter lifecycle, opaque handles, 16K BOs, fence errors/timeouts, map/alloc rollback; no GPU execution asserted.");
    puts("PASS: surface plane offsets, zero-copy aliasing, shared-busy/error propagation, import rollback and retained-reference cleanup.");
}
