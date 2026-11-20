/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Viva/Mesa transport for the existing, owned NativeMali connection.
 * No kernel impersonation, mock execution, CPU rendering, or automatic reopen.
 * One queue is submitted synchronously; syncobjs retain the actual GPU fence.
 * Original IOSurface pages are pinned through their task mapping, never copied. */
#include "vgpu.h"
#include "mali_native_wire.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NM_BOS 64
#define NM_SYNCS 256
#define NM_MAX_ARRAY 4096
#define NM_SECOND UINT64_C(1000000000)
struct fence { uint64_t native; unsigned refs; int done,error; };
struct bo {
    uint32_t id,flags,token;
    uint64_t native,va,size;
    void *cpu;
    size_t cpu_size;
    struct fence *fence;
    uint64_t in_page;
    void *surface;
};
struct syncobj { uint32_t id; int signaled; struct fence *fence; };
struct connection {
    const struct nm_transport *t;
    void *context;
    pthread_mutex_t mutex;
    struct nm_query caps;
    uint32_t gpu_id,next_bo,next_sync;
    struct bo bos[NM_BOS];
    struct syncobj syncs[NM_SYNCS];
    struct fence *last;
};

static void fence_set(struct fence **to,struct fence *from)
{
    if(from)++from->refs;
    if(*to && --(*to)->refs==0)free(*to);
    *to=from;
}
static struct bo *find_bo(struct connection *c,uint32_t id)
{
    for(unsigned i=0;id && i<NM_BOS;i++)if(c->bos[i].id==id)return &c->bos[i];
    return NULL;
}
static struct syncobj *find_sync(struct connection *c,uint32_t id)
{
    for(unsigned i=0;id && i<NM_SYNCS;i++)if(c->syncs[i].id==id)return &c->syncs[i];
    return NULL;
}
static int call(struct connection *c,uint32_t n,const void *in,size_t ni,void *out,size_t no)
{ return c->t->call(c->context,n,in,ni,out,no); }
static int wait_fence(struct connection *c,struct fence *f,uint64_t deadline)
{
    if(!f)return 0;
    while(!f->done) {
        uint64_t now=c->t->now_ns(c->context);
        uint64_t left=deadline>now?deadline-now:0;
        struct nm_wait w={f->native,left>NM_SECOND?NM_SECOND:left};
        int r=call(c,NM_WAIT,&w,sizeof(w),NULL,0);
        if(r==-ETIMEDOUT) {
            if(c->t->now_ns(c->context)>=deadline || !left)return r;
            continue;
        }
        f->error=r;f->done=1;
    }
    return f->error;
}
static uint64_t deadline(struct connection *c,int64_t absolute)
{ (void)c;return absolute<0?UINT64_MAX:(uint64_t)absolute; }
static int wait_sync(struct connection *c,struct syncobj *s,uint64_t until)
{
    uint32_t id=s->id;
    for(;;) {
        if(s->id!=id)return -ENOENT;
        if(s->fence)return wait_fence(c,s->fence,until);
        if(s->signaled)return 0;
        if(c->t->now_ns(c->context)>=until)return -ETIMEDOUT;
        /* SIGNAL/RESET from another user thread must be able to acquire it. */
        pthread_mutex_unlock(&c->mutex);
        struct timespec pause={0,100000};nanosleep(&pause,NULL);
        pthread_mutex_lock(&c->mutex);
    }
}
static int drain(struct connection *c)
{
    uint64_t now=c->t->now_ns(c->context);
    return wait_fence(c,c->last,now>UINT64_MAX-NM_SECOND?UINT64_MAX:now+NM_SECOND);
}
static int wait_shared(struct connection *c,struct bo *b,uint64_t until)
{
    struct nm_handle shared={b->native};
    for(;;){
        int r=call(c,NM_WAIT_SHARED,&shared,sizeof(shared),NULL,0);
        if(r!=-EBUSY)return r;
        if(c->t->now_ns(c->context)>=until)return -ETIMEDOUT;
        struct timespec pause={0,100000};nanosleep(&pause,NULL);
    }
}
static uint64_t allocate_va(struct connection *c,uint64_t bytes)
{
    const uint64_t end=c->caps.ias<32?UINT64_C(1)<<c->caps.ias:UINT64_C(1)<<32;
    uint64_t va=VGPU_VA_START;
    for(unsigned step=0;step<=NM_BOS;step++) {
        int overlap=0;
        if(va>=end || bytes>end-va)return 0;
        for(unsigned i=0;i<NM_BOS;i++) {
            const struct bo *b=&c->bos[i];
            if(b->id && va<b->va+b->size && b->va<va+bytes) {
                va=b->va+b->size;overlap=1;break;
            }
        }
        if(!overlap)return va;
    }
    return 0;
}
static int create_bo(struct connection *c,struct vgpu_create_bo *a)
{
    if(!a->size || a->pad || (a->flags&~(VGPU_BO_NOEXEC|VGPU_BO_WB_MMAP)))return -EINVAL;
    if(!c->next_bo)return -ENOSPC;
    struct bo *b=NULL;
    for(unsigned i=0;i<NM_BOS;i++)if(!c->bos[i].id){b=&c->bos[i];break;}
    if(!b)return -ENOSPC;
    int r=drain(c);if(r)return r;
    uint64_t page=c->t->page_size;
    uint64_t bytes=((uint64_t)a->size+page-1)&~(page-1);
    if(bytes>c->caps.max_bytes)return -E2BIG;
    uint64_t va=allocate_va(c,bytes);if(!va)return -ENOSPC;
    struct nm_alloc alloc={bytes};struct nm_handle h={0};
    r=call(c,NM_ALLOC,&alloc,sizeof(alloc),&h,sizeof(h));if(r)return r;
    if(!h.handle)return -EPROTO;
    struct nm_map map={h.handle,va,3u|((a->flags&VGPU_BO_NOEXEC)?4u:0u),0};
    struct nm_va mapped={0};
    r=call(c,NM_MAP,&map,sizeof(map),&mapped,sizeof(mapped));
    if(r || mapped.va!=va) {
        int release=call(c,NM_FREE,&h,sizeof(h),NULL,0);
        return r?r:(release?release:-EPROTO);
    }
    *b=(struct bo){.id=c->next_bo++,.flags=a->flags,.native=h.handle,.va=va,.size=bytes};
    a->handle=b->id;a->offset=va;
    return 0;
}
static int close_bo(struct connection *c,struct bo *b)
{
    if(!b)return -ENOENT;
    if(b->cpu)return -EBUSY;
    int r=drain(c);if(r)return r;
    struct nm_handle h={b->native};r=call(c,NM_FREE,&h,sizeof(h),NULL,0);
    if(!r){
        if(b->surface)c->t->release_surface(b->surface);
        fence_set(&b->fence,NULL);memset(b,0,sizeof(*b));
    }
    return r;
}
static int import_surface(struct connection *c,struct vgpu_import_iosurface *a)
{
    if(!a->surface_id || a->handle || a->pad || a->offset || a->size ||
        a->bytes_per_row || a->pixel_format || a->width || a->height)return -EINVAL;
    if(!c->t->surface || !c->t->release_surface)return -ENOTSUP;
    if(!c->next_bo)return -ENOSPC;
    struct bo *b=NULL;
    for(unsigned i=0;i<NM_BOS;i++)if(!c->bos[i].id){b=&c->bos[i];break;}
    if(!b)return -ENOSPC;
    int r=drain(c);if(r)return r;
    struct nm_surface_view surface={0};
    r=c->t->surface(c->context,a->surface_id,a->plane,&surface);if(r)return r;
    uint64_t page=c->t->page_size;
    if(!surface.retained || !surface.address || (surface.address&(page-1)) ||
        !surface.bytes || (surface.bytes&(page-1)) || surface.bytes>c->caps.max_bytes ||
        surface.bytes>UINT64_MAX-surface.address || surface.in_page>=page ||
        !surface.plane_bytes || surface.plane_bytes>surface.bytes-surface.in_page ||
        !surface.width || !surface.height || !surface.bytes_per_row){r=-EPROTO;goto failed;}
    uint64_t va=allocate_va(c,surface.bytes);if(!va){r=-ENOSPC;goto failed;}
    struct nm_import_user request={surface.address,surface.bytes,0,0};struct nm_handle h={0};
    r=call(c,NM_IMPORT_USER,&request,sizeof(request),&h,sizeof(h));if(r)goto failed;
    if(!h.handle){r=-EPROTO;goto failed;}
    struct nm_map mapping={h.handle,va,7,0};struct nm_va mapped={0};
    r=call(c,NM_MAP,&mapping,sizeof(mapping),&mapped,sizeof(mapped));
    if(r || mapped.va!=va){
        int release=call(c,NM_FREE,&h,sizeof(h),NULL,0);
        r=r?r:(release?release:-EPROTO);goto failed;
    }
    *b=(struct bo){.id=c->next_bo++,.flags=VGPU_BO_NOEXEC,.native=h.handle,
        .va=va,.size=surface.bytes,.in_page=surface.in_page,.surface=surface.retained};
    a->handle=b->id;a->offset=va+surface.in_page;a->size=surface.plane_bytes;
    a->bytes_per_row=surface.bytes_per_row;a->pixel_format=surface.pixel_format;
    a->width=surface.width;a->height=surface.height;return 0;
failed:
    if(surface.retained)c->t->release_surface(surface.retained);
    return r;
}
static int submit(struct connection *c,struct vgpu_submit *a)
{
    if(!a->jc || (a->jc&63) || a->seq || a->jm_ctx_handle>1 ||
       (a->requirements&~VGPU_JD_REQ_FS) || a->in_sync_count>NM_MAX_ARRAY ||
       !a->bo_handle_count || a->bo_handle_count>NM_BOS || !a->bo_handles ||
       (a->in_sync_count && !a->in_syncs))return -EINVAL;
    struct syncobj *out=a->out_sync?find_sync(c,a->out_sync):NULL;
    if(a->out_sync && !out)return -ENOENT;
    const uint32_t *handles=(const void *)(uintptr_t)a->bo_handles;
    const uint32_t *syncs=(const void *)(uintptr_t)a->in_syncs;
    int covered=0;
    for(unsigned i=0;i<a->bo_handle_count;i++) {
        struct bo *b=find_bo(c,handles[i]);if(!b)return -ENOENT;
        if(a->jc>=b->va && a->jc-b->va<=b->size-64)covered=1;
    }
    if(!covered)return -EINVAL;
    uint64_t until=c->t->now_ns(c->context)+NM_SECOND;
    for(unsigned i=0;i<a->in_sync_count;i++) {
        struct syncobj *s=find_sync(c,syncs[i]);if(!s)return -ENOENT;
        int r=wait_sync(c,s,until);if(r)return r;
    }
    int r=drain(c);if(r)return r;
    /* wait_sync releases the mutex: validate identities again before retaining
     * native fences or touching a BO another CPU thread could have closed. */
    out=a->out_sync?find_sync(c,a->out_sync):NULL;
    if(a->out_sync && !out)return -ENOENT;
    for(unsigned i=0;i<a->bo_handle_count;i++)if(!find_bo(c,handles[i]))return -ENOENT;
    struct fence *f=calloc(1,sizeof(*f));if(!f)return -ENOMEM;
    struct nm_submit req={a->jc,0,0,0,0};struct nm_fence_wire result={0};
    r=call(c,NM_SUBMIT,&req,sizeof(req),&result,sizeof(result));
    if(r || !result.fence){free(f);return r?r:-EPROTO;}
    f->native=result.fence;
    fence_set(&c->last,f);
    for(unsigned i=0;i<a->bo_handle_count;i++)fence_set(&find_bo(c,handles[i])->fence,f);
    if(out){out->signaled=0;fence_set(&out->fence,f);}
    return wait_fence(c,f,until);
}

static int dispatch(struct connection *c,enum vgpu_selector sel,void *arg,size_t bytes)
{
#define ARG(type) if(bytes!=sizeof(struct type))return -EINVAL; struct type *a=arg
    switch(sel) {
    case VGPU_SEL_GET_VERSION: {
        ARG(vgpu_version);memset(a,0,sizeof(*a));a->major=1;a->minor=0;
        a->gpu_id=c->gpu_id;a->page_size=c->t->page_size;
        memcpy(a->name,"Mali-G52 NativeMali",19);return 0;
    }
    case VGPU_SEL_GET_PARAM: {
        ARG(vgpu_get_param);if(a->pad)return -EINVAL;
        struct nm_param p={a->param,0,0},reply={0};
        int r=call(c,NM_PARAM,&p,sizeof(p),&reply,sizeof(reply));
        if(!r){if(reply.parameter!=a->param || reply.reserved)return -EPROTO;a->value=reply.value;}
        return r;
    }
    case VGPU_SEL_CREATE_BO:{ARG(vgpu_create_bo);return create_bo(c,a);}
    case VGPU_SEL_MMAP_BO:{ARG(vgpu_mmap_bo);if(a->flags)return -EINVAL;
        if(!find_bo(c,a->handle))return -ENOENT;a->offset=a->handle;return 0;}
    case VGPU_SEL_GET_BO_OFFSET:{ARG(vgpu_get_bo_offset);struct bo *b=find_bo(c,a->handle);
        if(a->pad)return -EINVAL;if(!b)return -ENOENT;a->offset=b->va+b->in_page;return 0;}
    case VGPU_SEL_GEM_CLOSE:{ARG(vgpu_gem_close);if(a->pad)return -EINVAL;return close_bo(c,find_bo(c,a->handle));}
    case VGPU_SEL_SUBMIT:{ARG(vgpu_submit);return submit(c,a);}
    case VGPU_SEL_WAIT_BO:{ARG(vgpu_wait_bo);struct bo *b=find_bo(c,a->handle);
        if(a->pad)return -EINVAL;if(!b)return -ENOENT;
        int r=wait_fence(c,b->fence,deadline(c,a->timeout_ns));
        if(r || !b->surface)return r;
        return wait_shared(c,b,deadline(c,a->timeout_ns));}
    case VGPU_SEL_QUERY_BO_INFO:{ARG(vgpu_query_bo_info);struct bo *b=find_bo(c,a->handle);
        if(a->pad)return -EINVAL;if(!b)return -ENOENT;a->extra_flags=b->surface?VGPU_BO_IS_IMPORTED:0;a->create_flags=b->flags;return 0;}
    case VGPU_SEL_IMPORT_IOSURFACE:{ARG(vgpu_import_iosurface);return import_surface(c,a);}
    case VGPU_SEL_MADVISE:{ARG(vgpu_madvise);if(!find_bo(c,a->handle))return -ENOENT;
        if(a->madv>1)return -EINVAL;a->retained=1;return 0;}
    case VGPU_SEL_SYNC_BO:{
        ARG(vgpu_sync_bo);if(a->pad || !a->ops || !a->op_count || a->op_count>NM_MAX_ARRAY)return -EINVAL;
        const struct vgpu_bo_sync_op *ops=(const void *)(uintptr_t)a->ops;
        for(unsigned i=0;i<a->op_count;i++){
            struct bo *b=find_bo(c,ops[i].handle);
            if(!b)return -ENOENT;
            if(ops[i].type>1 || !ops[i].size || ops[i].offset>=b->size-b->in_page ||
               ops[i].size>b->size-b->in_page-ops[i].offset)return -EINVAL;
        }
        int r=drain(c);if(r)return r;
        for(unsigned i=0;i<a->op_count;i++){
            struct bo *b=find_bo(c,ops[i].handle);
            struct nm_cache req={b->native,b->in_page+ops[i].offset,ops[i].size,ops[i].type,0};
            r=call(c,NM_CACHE_SYNC,&req,sizeof(req),NULL,0);if(r)return r;
        }
        return 0;
    }
    case VGPU_SEL_JM_CTX_CREATE:{ARG(vgpu_jm_ctx);if(a->priority!=1)return -ENOTSUP;a->handle=1;return 0;}
    case VGPU_SEL_JM_CTX_DESTROY:{ARG(vgpu_jm_ctx);return a->handle==1?drain(c):-ENOENT;}
    case VGPU_SEL_SYNCOBJ_CREATE:{
        ARG(vgpu_syncobj_create);if(a->flags&~VGPU_SYNCOBJ_CREATE_SIGNALED)return -EINVAL;
        if(!c->next_sync)return -ENOSPC;
        for(unsigned i=0;i<NM_SYNCS;i++)if(!c->syncs[i].id){
            c->syncs[i]=(struct syncobj){.id=c->next_sync++,.signaled=!!a->flags};a->handle=c->syncs[i].id;return 0;
        }
        return -ENOSPC;
    }
    case VGPU_SEL_SYNCOBJ_DESTROY:{ARG(vgpu_syncobj_destroy);if(a->pad)return -EINVAL;
        struct syncobj *s=find_sync(c,a->handle);if(!s)return -ENOENT;
        fence_set(&s->fence,NULL);memset(s,0,sizeof(*s));return 0;}
    case VGPU_SEL_SYNCOBJ_SIGNAL:case VGPU_SEL_SYNCOBJ_RESET:{
        ARG(vgpu_syncobj_array);if(a->pad || !a->handles || !a->count_handles || a->count_handles>NM_MAX_ARRAY)return -EINVAL;
        const uint32_t *ids=(const void *)(uintptr_t)a->handles;
        for(unsigned i=0;i<a->count_handles;i++)if(!find_sync(c,ids[i]))return -ENOENT;
        for(unsigned i=0;i<a->count_handles;i++){
            struct syncobj *s=find_sync(c,ids[i]);fence_set(&s->fence,NULL);s->signaled=sel==VGPU_SEL_SYNCOBJ_SIGNAL;
        }return 0;
    }
    case VGPU_SEL_SYNCOBJ_TRANSFER:{ARG(vgpu_syncobj_transfer);
        struct syncobj *from=find_sync(c,a->src),*to=find_sync(c,a->dst);
        if(!from || !to)return -ENOENT;
        if(!from->fence && !from->signaled)return -EINVAL;
        fence_set(&to->fence,from->fence);to->signaled=from->signaled;return 0;}
    case VGPU_SEL_SYNCOBJ_WAIT:{
        ARG(vgpu_syncobj_wait);if(a->pad || !a->handles || !a->count_handles || a->count_handles>NM_MAX_ARRAY ||
            (a->flags&~(VGPU_SYNCOBJ_WAIT_ALL|VGPU_SYNCOBJ_WAIT_FOR_SUBMIT)))return -EINVAL;
        const uint32_t *ids=(const void *)(uintptr_t)a->handles;
        uint64_t until=deadline(c,a->timeout_nsec);
        for(;;){
            unsigned ready=0,first=UINT32_MAX;
            for(unsigned i=0;i<a->count_handles;i++){
                struct syncobj *s=find_sync(c,ids[i]);if(!s)return -ENOENT;
                if(!s->fence && !s->signaled && !(a->flags&VGPU_SYNCOBJ_WAIT_FOR_SUBMIT))return -EINVAL;
                int r=wait_sync(c,s,c->t->now_ns(c->context));
                if(!r){++ready;if(first==UINT32_MAX)first=i;}
                else if(r!=-ETIMEDOUT)return r;
            }
            if((a->flags&VGPU_SYNCOBJ_WAIT_ALL)?ready==a->count_handles:ready>0){a->first_signaled=first;return 0;}
            if(c->t->now_ns(c->context)>=until)return -ETIMEDOUT;
            pthread_mutex_unlock(&c->mutex);struct timespec pause={0,100000};nanosleep(&pause,NULL);pthread_mutex_lock(&c->mutex);
        }
    }
    default:return -ENOTSUP;
    }
#undef ARG
}
static int native_ioctl(void *context,enum vgpu_selector sel,void *arg,size_t bytes)
{
    struct connection *c=context;if(!c || !arg)return -EINVAL;
    pthread_mutex_lock(&c->mutex);int r=dispatch(c,sel,arg,bytes);pthread_mutex_unlock(&c->mutex);return r;
}
static void *native_mmap(void *context,uint64_t token,size_t bytes)
{
    struct connection *c=context;pthread_mutex_lock(&c->mutex);
    int r=0;void *p=NULL;
    struct bo *b=token<=UINT32_MAX?find_bo(c,(uint32_t)token):NULL;
    if(!b){r=-ENOENT;goto done;}
    if(!bytes || bytes>b->size){r=-EINVAL;goto done;}
    if(b->cpu){r=-EBUSY;goto done;}
    r=drain(c);if(r)goto done;
    struct nm_handle h={b->native};struct nm_cpu_map reply={0};
    r=call(c,NM_CPU_MAP,&h,sizeof(h),&reply,sizeof(reply));if(r)goto done;
    if(!reply.token || reply.reserved || reply.bytes!=b->size){r=-EPROTO;goto done;}
    b->token=reply.token;
    size_t mapped=(size_t)reply.bytes;
    r=c->t->map(c->context,b->token,&p,&mapped);
    if(!r && (!p || mapped<bytes)){
        if(p)c->t->unmap(c->context,b->token,p);
        r=-EPROTO;
    }
    if(r){
        struct nm_cpu_unmap unmap={b->native,b->token,0};call(c,NM_CPU_UNMAP,&unmap,sizeof(unmap),NULL,0);
        b->token=0;p=NULL;
    }else{b->cpu=p;b->cpu_size=mapped;}
done:
    pthread_mutex_unlock(&c->mutex);if(r)errno=-r;return p;
}
static int native_munmap(void *context,void *p,size_t bytes)
{
    struct connection *c=context;pthread_mutex_lock(&c->mutex);int r=-ENOENT;
    for(unsigned i=0;i<NM_BOS;i++){
        struct bo *b=&c->bos[i];if(!b->id || !b->cpu || b->cpu!=p)continue;
        if(!bytes || bytes>b->cpu_size){r=-EINVAL;break;}
        r=drain(c);if(r)break;
        r=c->t->unmap(c->context,b->token,b->cpu);if(r)break;
        b->cpu=NULL;b->cpu_size=0;
        struct nm_cpu_unmap unmap={b->native,b->token,0};r=call(c,NM_CPU_UNMAP,&unmap,sizeof(unmap),NULL,0);
        if(!r)b->token=0;
        break;
    }
    pthread_mutex_unlock(&c->mutex);return r;
}
static void native_close(void *context)
{
    struct connection *c=context;if(!c)return;
    for(unsigned i=0;i<NM_BOS;i++)if(c->bos[i].cpu)
        c->t->unmap(c->context,c->bos[i].token,c->bos[i].cpu);
    /* The kernel waits/quiesces before reclaiming this connection's allocations;
     * unrecoverable DMA is quarantined there, never freed through a fake fence. */
    c->t->close(c->context);
    for(unsigned i=0;i<NM_BOS;i++){
        if(c->bos[i].surface)c->t->release_surface(c->bos[i].surface);
        fence_set(&c->bos[i].fence,NULL);
    }
    for(unsigned i=0;i<NM_SYNCS;i++)fence_set(&c->syncs[i].fence,NULL);
    fence_set(&c->last,NULL);pthread_mutex_destroy(&c->mutex);free(c);
}
int nm_backend_connect(const struct nm_transport *t,void *context,void **out)
{
    if(!out || !t || !t->call || !t->map || !t->unmap || !t->close || !t->now_ns ||
       t->page_size<4096 || (t->page_size&(t->page_size-1)))return -EINVAL;
    *out=NULL;
    struct connection *c=calloc(1,sizeof(*c));if(!c){t->close(context);return -ENOMEM;}
    c->t=t;c->context=context;c->next_bo=c->next_sync=1;
    if(pthread_mutex_init(&c->mutex,NULL)){free(c);t->close(context);return -ENOMEM;}
    struct nm_query request={.version=2};
    int r=call(c,NM_QUERY,&request,sizeof(request),&c->caps,sizeof(c->caps));
    if(!r && (c->caps.version!=2 || c->caps.flags || c->caps.ias<26 || c->caps.ias>48 ||
              c->caps.oas<32 || c->caps.oas>48 || c->caps.max_bytes<16384))r=-EPROTO;
    struct nm_param param={0},reply={0};
    if(!r)r=call(c,NM_PARAM,&param,sizeof(param),&reply,sizeof(reply));
    if(!r && (reply.value!=0x7402 || reply.parameter || reply.reserved))r=-ENODEV;
    if(!r){c->gpu_id=(uint32_t)reply.value<<16;param.parameter=1;
        r=call(c,NM_PARAM,&param,sizeof(param),&reply,sizeof(reply));
        if(!r && (reply.parameter!=1 || reply.reserved || reply.value>0xffff))r=-EPROTO;
        if(!r)c->gpu_id|=(uint32_t)reply.value;
    }
    if(r){native_close(c);return r;}
    *out=c;return 0;
}

/* Implemented by the Darwin file; the test supplies an owned transport instead. */
extern int nm_iokit_open(void **);
const struct vgpu_backend vgpu_backend_iokit={"native-mali",nm_iokit_open,native_close,native_ioctl,native_mmap,native_munmap};
