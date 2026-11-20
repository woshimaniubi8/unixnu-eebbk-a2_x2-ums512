/* SPDX-License-Identifier: MIT */
/* Explicit owner probe. It does not register an accelerator or replace a
 * system process. Results require Metal pipelines AND hardware submissions. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
@interface NSObject (NativeMaliDevice)
- (instancetype)initWithDriver:(const char *)driver;
@end
static int completed(id<MTLCommandBuffer> cb)
{
    [cb commit];[cb waitUntilCompleted];
    if(cb.status!=MTLCommandBufferStatusCompleted){
        fprintf(stderr,"nm metal: command failed status=%lu error=%s\n",(unsigned long)cb.status,cb.error.description.UTF8String);
        return 0;
    }return 1;
}
int main(int argc,char **argv) { @autoreleasepool {
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    if(argc!=3){fprintf(stderr,"usage: native-metal-probe BUNDLE SHADERS.metallib\n");return 64;}
    NSBundle *bundle=[NSBundle bundleWithPath:@(argv[1])];NSError *error=nil;
    if(![bundle loadAndReturnError:&error]){fprintf(stderr,"nm metal: bundle %s\n",error.description.UTF8String);return 1;}
    void *image=dlopen(bundle.executablePath.fileSystemRepresentation,RTLD_NOW|RTLD_LOCAL);
    const char *(*backend)(void)=image?dlsym(image,"vgpu_backend_name"):NULL;
    void (*counters)(uint64_t *,uint64_t *)=image?dlsym(image,"vgpu_counters"):NULL;
    if(!backend || !counters || strcmp(backend(),"native-mali")){fprintf(stderr,"nm metal: physical backend missing\n");return 2;}
    Class cls=[bundle classNamed:@"VMDevice"];
    id<MTLDevice> device=cls?[[cls alloc] initWithDriver:"panfrost"]:nil;
    if(!device){fprintf(stderr,"nm metal: real Panfrost device init failed\n");return 3;}
    printf("nm metal: device=%s backend=%s\n",device.name.UTF8String,backend());
    id<MTLLibrary> lib=[device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&error];
    if(!lib){fprintf(stderr,"nm metal: metallib %s\n",error.description.UTF8String);return 4;}
    id<MTLCommandQueue> queue=[device newCommandQueue];
    id<MTLFunction> kernel=[lib newFunctionWithName:@"nm_double"];
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
    if(!completed(cb))return 8;
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
    id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    if(!texture || !pipeline){fprintf(stderr,"nm metal: render pipeline %s\n",error.description.UTF8String);return 10;}
    MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture=texture;rp.colorAttachments[0].loadAction=MTLLoadActionClear;
    rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,1,1);
    cb=[queue commandBuffer];id<MTLRenderCommandEncoder> render=[cb renderCommandEncoderWithDescriptor:rp];
    if(!cb || !render)return 11;
    [render setRenderPipelineState:pipeline];[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [render endEncoding];if(!completed(cb))return 12;
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
    printf("nm metal: PASS compute=64 render=4096 red=%u blue=%u hardware_jobs=%llu\n",red,blue,(unsigned long long)(jobs1-jobs0));
    return 0;
} }
