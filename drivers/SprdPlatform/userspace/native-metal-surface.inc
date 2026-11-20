/* SPDX-License-Identifier: MIT */
/* Qualification uses original IOSurface Mach rights, not a replacement root. */
#include <IOSurface/IOSurfaceRef.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
extern char **environ;
// iOS's SDK omits launchd's public bootstrap header. These declarations match
// Apple launchd/liblaunch/bootstrap.h; implementations remain in libSystem.
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_check_in(mach_port_t,const char *,mach_port_t *);
extern kern_return_t bootstrap_look_up(mach_port_t,const char *,mach_port_t *);
#define NM_SURFACE_SERVICE "org.unixnu.bbk.mali-metal.surface"
enum { NmSurfaceRequest=0x4d188001, NmSurfaceReply=0x4d188002 };
struct NmSurfaceMessage {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t surface;
    mach_msg_trailer_t trailer;
};
static IOSurfaceRef receiveSurface(uint32_t expected)
{
    mach_port_t server=MACH_PORT_NULL,reply=MACH_PORT_NULL;
    if(bootstrap_look_up(bootstrap_port,NM_SURFACE_SERVICE,&server)!=KERN_SUCCESS)return NULL;
    if(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&reply)!=KERN_SUCCESS){
        mach_port_deallocate(mach_task_self(),server);return NULL;
    }
    mach_msg_header_t request={0};
    request.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND_ONCE);
    request.msgh_size=sizeof(request);request.msgh_remote_port=server;
    request.msgh_local_port=reply;request.msgh_id=NmSurfaceRequest;
    kern_return_t kr=mach_msg(&request,MACH_SEND_MSG|MACH_SEND_TIMEOUT,
        sizeof(request),0,MACH_PORT_NULL,5000,MACH_PORT_NULL);
    mach_port_deallocate(mach_task_self(),server);
    struct NmSurfaceMessage response={0};
    if(kr==KERN_SUCCESS)kr=mach_msg(&response.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,
        0,sizeof(response),reply,300000,MACH_PORT_NULL);
    mach_port_destroy(mach_task_self(),reply);
    if(kr!=KERN_SUCCESS || response.header.msgh_id!=NmSurfaceReply ||
        !(response.header.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
        response.body.msgh_descriptor_count!=1 || response.surface.type!=MACH_MSG_PORT_DESCRIPTOR){
        if(kr==KERN_SUCCESS)mach_msg_destroy(&response.header);
        return NULL;
    }
    IOSurfaceRef surface=IOSurfaceLookupFromMachPort(response.surface.name);
    mach_port_deallocate(mach_task_self(),response.surface.name);
    if(surface && IOSurfaceGetID(surface)!=expected){CFRelease(surface);return NULL;}
    return surface;
}
static int sendSurface(mach_port_t server,IOSurfaceRef surface)
{
    struct {mach_msg_header_t header;mach_msg_trailer_t trailer;} request={0};
    kern_return_t kr=mach_msg(&request.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,
        0,sizeof(request),server,300000,MACH_PORT_NULL);
    if(kr!=KERN_SUCCESS)return 0;
    if(request.header.msgh_id!=NmSurfaceRequest ||
        request.header.msgh_size!=sizeof(mach_msg_header_t) ||
        (request.header.msgh_bits&MACH_MSGH_BITS_COMPLEX)){
        mach_msg_destroy(&request.header);return 0;
    }
    mach_port_t port=IOSurfaceCreateMachPort(surface);
    if(port==MACH_PORT_NULL){mach_port_deallocate(mach_task_self(),request.header.msgh_remote_port);return 0;}
    struct NmSurfaceMessage response={0};
    response.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE,0)|MACH_MSGH_BITS_COMPLEX;
    response.header.msgh_size=offsetof(struct NmSurfaceMessage,trailer);
    response.header.msgh_remote_port=request.header.msgh_remote_port;
    response.header.msgh_id=NmSurfaceReply;
    response.body.msgh_descriptor_count=1;
    response.surface.name=port;response.surface.disposition=MACH_MSG_TYPE_COPY_SEND;
    response.surface.type=MACH_MSG_PORT_DESCRIPTOR;
    kr=mach_msg(&response.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,
        response.header.msgh_size,0,MACH_PORT_NULL,5000,MACH_PORT_NULL);
    mach_port_deallocate(mach_task_self(),port);
    return kr==KERN_SUCCESS;
}
static int surfacePixels(const uint8_t *pixels,unsigned *red,unsigned *blue)
{
    *red=*blue=0;
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
        const uint8_t *p=pixels+(y*64+x)*4;
        int r=p[0]==255 && p[1]==0 && p[2]==0 && p[3]==255;
        int b=p[0]==0 && p[1]==0 && p[2]==255 && p[3]==255;
        if(!r && !b)return 0;
        *red+=r;*blue+=b;
        if((x<3 || x>60 || y<3 || y>60) && !b)return 0;
        if(x>27 && x<36 && y>27 && y<36 && !r)return 0;
    }
    return *red>=500 && *blue>=500 && *red+*blue==4096;
}
static int surfaceChild(id<MTLDevice> device,id<MTLLibrary> library,id<MTLCommandQueue> queue,
    uint32_t expected,void (*counters)(uint64_t *,uint64_t *))
{
    printf("nm surface-child: receive Mach right begin id=%u\n",expected);
    IOSurfaceRef surface=receiveSurface(expected);
    if(!surface){fprintf(stderr,"nm surface-child: original IOSurface right transfer failed\n");return 20;}
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:surface plane:0];
    id<MTLBuffer> output=[device newBufferWithLength:16384 options:MTLResourceStorageModeShared];
    NSError *error=nil;
    id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"nm_read_surface"] error:&error];
    if(!texture || !output || !pipeline){CFRelease(surface);fprintf(stderr,"nm surface-child: actual import or pipeline rejected\n");return 21;}
    uint64_t values[64]={0},jobs0=0,jobs1=0;counters(values,&jobs0);
    id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];
    [enc setComputePipelineState:pipeline];[enc setTexture:texture atIndex:0];[enc setBuffer:output offset:0 atIndex:0];
    [enc dispatchThreadgroups:MTLSizeMake(128,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[enc endEncoding];
    if(!enc || !completed(cb)){CFRelease(surface);return 22;}
    counters(values,&jobs1);
    unsigned red=0,blue=0;int valid=surfacePixels(output.contents,&red,&blue);
    CFRelease(surface);
    if(!valid || jobs1<=jobs0)return 23;
    printf("nm surface-child: PASS id=%u pixels=4096 red=%u blue=%u hardware_jobs=%llu\n",
        expected,red,blue,(unsigned long long)(jobs1-jobs0));
    return 0;
}
static int surfaceParent(id<MTLDevice> device,id<MTLCommandQueue> queue,
    id<MTLRenderPipelineState> pipeline,const char *program,const char *bundle,const char *metallib)
{
    NSDictionary *properties=@{(__bridge id)kIOSurfaceWidth:@64,(__bridge id)kIOSurfaceHeight:@64,
        (__bridge id)kIOSurfaceBytesPerElement:@4,(__bridge id)kIOSurfaceBytesPerRow:@256,
        (__bridge id)kIOSurfacePixelFormat:@0x52474241,(__bridge id)kIOSurfaceAllocSize:@32768};
    IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if(!surface){fprintf(stderr,"nm surface: original IOSurfaceCreate failed\n");return 24;}
    size_t bytes=IOSurfaceGetAllocSize(surface);void *base=IOSurfaceGetBaseAddress(surface);
    if(bytes<32768 || !base || IOSurfaceGetBytesPerRow(surface)!=256 ||
        IOSurfaceLock(surface,0,NULL)!=KERN_SUCCESS){CFRelease(surface);return 25;}
    memset(base,0x6b,bytes);IOSurfaceUnlock(surface,0,NULL);
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:surface plane:0];
    if(!texture){CFRelease(surface);fprintf(stderr,"nm surface: zero-copy import rejected\n");return 26;}
    MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture=texture;rp.colorAttachments[0].loadAction=MTLLoadActionClear;
    rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,1,1);
    id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLRenderCommandEncoder> render=[cb renderCommandEncoderWithDescriptor:rp];
    if(!render){CFRelease(surface);return 27;}
    [render setRenderPipelineState:pipeline];[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[render endEncoding];
    printf("nm surface: render to original pages id=%u bytes=%lu\n",IOSurfaceGetID(surface),(unsigned long)bytes);
    if(!completed(cb) || IOSurfaceLock(surface,kIOSurfaceLockReadOnly,NULL)!=KERN_SUCCESS){CFRelease(surface);return 28;}
    unsigned red=0,blue=0;int valid=surfacePixels(base,&red,&blue);
    for(size_t i=16384;i<bytes;i++)if(((const uint8_t *)base)[i]!=0x6b){valid=0;break;}
    IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL);
    if(!valid){CFRelease(surface);fprintf(stderr,"nm surface: original page pixels or tail guard failed\n");return 29;}
    mach_port_t server=MACH_PORT_NULL;
    kern_return_t kr=bootstrap_check_in(bootstrap_port,NM_SURFACE_SERVICE,&server);
    if(kr!=KERN_SUCCESS){CFRelease(surface);fprintf(stderr,"nm surface: owner Mach service check-in kr=0x%x\n",kr);return 30;}
    char id[32];snprintf(id,sizeof(id),"%u",IOSurfaceGetID(surface));
    char *args[]={(char *)program,"--surface-child",id,(char *)bundle,(char *)metallib,NULL};
    pid_t child=0;int status=0,spawn=posix_spawn(&child,program,NULL,NULL,args,environ);
    int sent=spawn==0 && sendSurface(server,surface);
    mach_port_destroy(mach_task_self(),server);
    if(spawn){CFRelease(surface);fprintf(stderr,"nm surface: child spawn error=%d\n",spawn);return 31;}
    if(!sent)kill(child,SIGKILL);
    struct timespec started;clock_gettime(CLOCK_MONOTONIC,&started);
    for(;;){
        pid_t done=waitpid(child,&status,WNOHANG);
        if(done==child)break;
        if(done<0 && errno!=EINTR){CFRelease(surface);return 32;}
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
        if(now.tv_sec-started.tv_sec>=300){kill(child,SIGKILL);waitpid(child,&status,0);CFRelease(surface);return 33;}
        struct timespec pause={0,100000000};nanosleep(&pause,NULL);
    }
    CFRelease(surface);
    if(!sent || !WIFEXITED(status) || WEXITSTATUS(status)!=0){fprintf(stderr,"nm surface: child status=0x%x sent=%d\n",status,sent);return 34;}
    printf("nm surface: PASS pixels=4096 cross_process=1 tail_guard=0 child_exit=0\n");
    return 0;
}
