/* SPDX-License-Identifier: MIT */
/* Explicit owner probe. It does not register an accelerator or replace a
 * system process. Results require Metal pipelines AND hardware submissions. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <objc/runtime.h>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
static thread_t probeThread;
static void *sampleProbe(void *unused)
{
    (void)unused;
    for(unsigned sample=0;sample<32;sample++){
        struct timespec delay={30,0};
        while(nanosleep(&delay,&delay)!=0){if(errno!=EINTR)return NULL;}
        thread_basic_info_data_t basic={0};
        mach_msg_type_number_t count=THREAD_BASIC_INFO_COUNT;
        kern_return_t info=thread_info(probeThread,THREAD_BASIC_INFO,(thread_info_t)&basic,&count);
        arm_thread_state64_t state={0};count=ARM_THREAD_STATE64_COUNT;
        kern_return_t kr=thread_get_state(probeThread,ARM_THREAD_STATE64,(thread_state_t)&state,&count);
        uintptr_t pc=kr==KERN_SUCCESS?(uintptr_t)arm_thread_state64_get_pc(state):0;
        uintptr_t lr=kr==KERN_SUCCESS?(uintptr_t)arm_thread_state64_get_lr(state):0;
        fprintf(stderr,"nm metal: sample=%u state_kr=0x%x info_kr=0x%x run=%d cpu=%d user=%d.%06d system=%d.%06d pc=0x%llx lr=0x%llx\n",
            sample,(unsigned)kr,(unsigned)info,basic.run_state,basic.cpu_usage,
            basic.user_time.seconds,basic.user_time.microseconds,basic.system_time.seconds,basic.system_time.microseconds,
            (unsigned long long)pc,(unsigned long long)lr);
        Dl_info image={0};
        if(pc && dladdr((const void *)pc,&image) && image.dli_fbase){
            fprintf(stderr,"nm metal: sample=%u image=%s rva=0x%llx symbol=%s\n",sample,
                image.dli_fname?image.dli_fname:"?",(unsigned long long)(pc-(uintptr_t)image.dli_fbase),image.dli_sname?image.dli_sname:"?");
        }
    }
    return NULL;
}
@interface NSObject (NativeMaliDevice)
- (instancetype)initWithDriver:(const char *)driver;
- (NSData *)bitcodeData;
@end
static int completed(id<MTLCommandBuffer> cb)
{
    [cb commit];[cb waitUntilCompleted];
    if(cb.status!=MTLCommandBufferStatusCompleted){
        fprintf(stderr,"nm metal: command failed status=%lu error=%s\n",(unsigned long)cb.status,cb.error.description.UTF8String);
        return 0;
    }return 1;
}
#include "native-metal-surface.inc"
int main(int argc,char **argv) {
    static const char started[]="nm metal: main enter\n";
    (void)write(2,started,sizeof(started)-1);
    @autoreleasepool {
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    printf("nm metal: Foundation ready\n");
    probeThread=mach_thread_self();
    pthread_t sampler;
    int samplingError=pthread_create(&sampler,NULL,sampleProbe,NULL);
    if(samplingError){fprintf(stderr,"nm metal: diagnostic thread error=%d\n",samplingError);return 70;}
    pthread_detach(sampler);
    int childMode=argc==5 && strcmp(argv[1],"--surface-child")==0;
    if(argc!=3 && !childMode){fprintf(stderr,"usage: native-metal-probe BUNDLE SHADERS.metallib\n");return 64;}
    const char *bundleArgument=argv[childMode?3:1],*shaderArgument=argv[childMode?4:2];
    uint32_t expectedSurface=0;
    if(childMode){
        char *end=NULL;errno=0;unsigned long id=strtoul(argv[2],&end,10);
        if(errno || !end || *end || !id || id>UINT32_MAX)return 64;
        expectedSurface=(uint32_t)id;
    }
    char executable[1024];
    if(setenv("TSMTL_LOG","1",1)!=0)return 64;
    int pathBytes=snprintf(executable,sizeof(executable),"%s/VivaMTLDriver",bundleArgument);
    if(pathBytes<0 || (size_t)pathBytes>=sizeof(executable))return 64;
    printf("nm metal: dlopen begin path=%s\n",executable);
    void *image=dlopen(executable,RTLD_NOW|RTLD_LOCAL);
    if(!image){fprintf(stderr,"nm metal: dlopen %s\n",dlerror());return 1;}
    printf("nm metal: bundle loaded\n");
    NSError *error=nil;
    const char *(*backend)(void)=image?dlsym(image,"vgpu_backend_name"):NULL;
    void (*counters)(uint64_t *,uint64_t *)=image?dlsym(image,"vgpu_counters"):NULL;
    if(!backend || !counters || strcmp(backend(),"native-mali")){fprintf(stderr,"nm metal: physical backend missing\n");return 2;}
    Class cls=objc_getClass("VMDevice");
    const char *classImage=cls?class_getImageName(cls):NULL;
    const char *imageName=classImage?strrchr(classImage,'/'):NULL;
    if(!imageName || strcmp(imageName+1,"VivaMTLDriver")){
        fprintf(stderr,"nm metal: VMDevice did not come from the loaded driver\n");return 3;
    }
    printf("nm metal: Panfrost device init begin\n");
    id<MTLDevice> device=cls?[[cls alloc] initWithDriver:"panfrost"]:nil;
    if(!device){fprintf(stderr,"nm metal: real Panfrost device init failed\n");return 3;}
    printf("nm metal: device=%s backend=%s\n",device.name.UTF8String,backend());
    printf("nm metal: metallib load begin\n");
    id<MTLLibrary> lib=[device newLibraryWithURL:[NSURL fileURLWithPath:@(shaderArgument)] error:&error];
    if(!lib){fprintf(stderr,"nm metal: metallib %s\n",error.description.UTF8String);return 4;}
    id<MTLCommandQueue> queue=[device newCommandQueue];
    if(!queue)return 5;
    if(childMode)return surfaceChild(device,lib,queue,expectedSurface,counters);
    id<MTLFunction> kernel=[lib newFunctionWithName:@"nm_double"];
    if(!kernel || ![(id)kernel respondsToSelector:@selector(bitcodeData)]){
        fprintf(stderr,"nm metal: real function AIR accessor unavailable\n");return 5;
    }
    printf("nm metal: AIR accessor begin\n");
    NSData *bitcode=[(id)kernel bitcodeData];
    printf("nm metal: AIR accessor returned bytes=%lu\n",(unsigned long)bitcode.length);
    if(!bitcode.length)return 5;
    printf("nm metal: compute pipeline begin\n");
    id<MTLComputePipelineState> compute=[device newComputePipelineStateWithFunction:kernel error:&error];
    if(!queue || !compute){fprintf(stderr,"nm metal: compute pipeline %s\n",error.description.UTF8String);return 5;}
    uint64_t before[64]={0},after[64]={0},jobs0=0,jobs1=0;counters(before,&jobs0);
    id<MTLBuffer> buffer=[device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if(!buffer || !buffer.contents)return 6;
    float *data=buffer.contents;for(unsigned i=0;i<64;i++)data[i]=(float)i;
    id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> enc=[cb computeCommandEncoder];
    if(!cb || !enc)return 7;
    [enc setComputePipelineState:compute];[enc setBuffer:buffer offset:0 atIndex:0];
    [enc dispatchThreadgroups:MTLSizeMake(2,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[enc endEncoding];
    printf("nm metal: compute submit begin\n");
    if(!completed(cb))return 8;
    printf("nm metal: compute completed\n");
    for(unsigned i=0;i<64;i++)if(data[i]!=(float)(2*i+1)){
        fprintf(stderr,"nm metal: compute mismatch index=%u value=%g\n",i,data[i]);return 9;
    }
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    id<MTLTexture> texture=[device newTextureWithDescriptor:td];
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
    pd.vertexFunction=[lib newFunctionWithName:@"nm_triangle"];
    pd.fragmentFunction=[lib newFunctionWithName:@"nm_red"];
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    printf("nm metal: render pipeline begin\n");
    id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    if(!texture || !pipeline){fprintf(stderr,"nm metal: render pipeline %s\n",error.description.UTF8String);return 10;}
    MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture=texture;rp.colorAttachments[0].loadAction=MTLLoadActionClear;
    rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,1,1);
    cb=[queue commandBuffer];id<MTLRenderCommandEncoder> render=[cb renderCommandEncoderWithDescriptor:rp];
    if(!cb || !render)return 11;
    [render setRenderPipelineState:pipeline];[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [render endEncoding];printf("nm metal: render submit begin\n");if(!completed(cb))return 12;
    printf("nm metal: render completed\n");
    uint8_t pixels[64*64*4];memset(pixels,0x6b,sizeof(pixels));
    [texture getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
    unsigned red=0,blue=0;
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
        uint8_t *p=pixels+(y*64+x)*4;
        int isRed=p[0]==255 && p[1]==0 && p[2]==0 && p[3]==255;
        int isBlue=p[0]==0 && p[1]==0 && p[2]==255 && p[3]==255;
        red+=isRed;blue+=isBlue;
        if((x<3 || x>60 || y<3 || y>60) && !isBlue)return 13;
        if(x>27 && x<36 && y>27 && y<36 && !isRed)return 14;
    }
    counters(after,&jobs1);
    if(red<500 || blue<500 || red+blue!=4096 || jobs1-jobs0<3){
        fprintf(stderr,"nm metal: incomplete readback red=%u blue=%u hardware_jobs=%llu\n",red,blue,(unsigned long long)(jobs1-jobs0));return 15;
    }
    int surfaceResult=surfaceParent(device,queue,pipeline,argv[0],bundleArgument,shaderArgument);
    if(surfaceResult)return surfaceResult;
    counters(after,&jobs1);
    printf("nm metal: PASS compute=64 render=4096 red=%u blue=%u hardware_jobs=%llu surface=4096 cross_process=1 guards=0\n",red,blue,(unsigned long long)(jobs1-jobs0));
    return 0;
} }
