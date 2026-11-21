/* SPDX-License-Identifier: MIT */
/* Observe each shipping Viva/PRIME import boundary on the exact same surface. */
#include <IOSurface/IOSurfaceRef.h>
#include <errno.h>
#include <unistd.h>
enum { NmVivaImportSurface=14, NmVivaCloseBo=5 };
struct NmVivaImportRecord {
    uint32_t surface_id,plane,handle,pad;
    uint64_t offset,size;
    uint32_t bytes_per_row,pixel_format,width,height;
};
_Static_assert(sizeof(struct NmVivaImportRecord)==48,"Pinned Viva v1 surface record");
static uint64_t surfaceImportCalls(void *image)
{
    void (*get)(uint64_t *,uint64_t *)=dlsym(image,"vgpu_counters");
    uint64_t calls[64]={0},jobs=0;
    if(get)get(calls,&jobs);
    return calls[NmVivaImportSurface];
}
static void surfaceBridgePreflight(void *image,IOSurfaceRef surface)
{
    printf("nm surface-bridge: id=%u base=0x%llx bytes=0x%llx row=%lu planes=%lu\n",
        IOSurfaceGetID(surface),(unsigned long long)(uintptr_t)IOSurfaceGetBaseAddress(surface),
        (unsigned long long)IOSurfaceGetAllocSize(surface),(unsigned long)IOSurfaceGetBytesPerRow(surface),
        (unsigned long)IOSurfaceGetPlaneCount(surface));
    int (*openDevice)(void)=dlsym(image,"vgpu_open");
    int (*closeDevice)(int)=dlsym(image,"vgpu_close");
    int (*call)(int,uint32_t,void *)=dlsym(image,"vgpu_ioctl");
    int (*surfaceFd)(int,uint32_t,unsigned)=dlsym(image,"vmg_drm_surface_fd");
    void (*closeSurfaceFd)(int)=dlsym(image,"vmg_drm_surface_fd_close");
    int (*primeToHandle)(int,int,uint32_t *)=dlsym(image,"drmPrimeFDToHandle");
    int (*primeSize)(int,uint64_t *)=dlsym(image,"vmg_drm_surface_size");
    int (*closeBo)(int,uint32_t)=dlsym(image,"drmCloseBufferHandle");
    if(!openDevice || !closeDevice || !call || !surfaceFd || !closeSurfaceFd || !primeToHandle || !closeBo || !primeSize){
        fprintf(stderr,"nm surface-bridge: actual frontend API export missing\n");return;
    }
    int fd=openDevice();
    printf("nm surface-bridge: vgpu_open fd=%d\n",fd);
    if(fd<0)return;
    struct NmVivaImportRecord request={.surface_id=IOSurfaceGetID(surface)};
    errno=0;int result=call(fd,NmVivaImportSurface,&request),error=errno;
    printf("nm surface-bridge: Viva import result=%d errno=%d handle=%u va=0x%llx bytes=0x%llx\n",
        result,error,request.handle,(unsigned long long)request.offset,(unsigned long long)request.size);
    if(!result && request.handle){
        struct {uint32_t handle,pad;} release={request.handle,0};
        printf("nm surface-bridge: Viva release result=%d\n",call(fd,NmVivaCloseBo,&release));
    }
    errno=0;int prime=surfaceFd(fd,IOSurfaceGetID(surface),0);error=errno;
    printf("nm surface-bridge: PRIME fd=%d errno=%d\n",prime,error);
    if(prime>=0){
        uint64_t size=0;int sized=primeSize(prime,&size);
        printf("nm surface-bridge: PRIME metadata size=0x%llx result=%d storage_bytes=0\n",(unsigned long long)size,sized);
        uint32_t handle=0;
        errno=0;result=primeToHandle(fd,prime,&handle);error=errno;
        printf("nm surface-bridge: PRIME import result=%d errno=%d handle=%u\n",result,error,handle);
        if(!result && handle)printf("nm surface-bridge: PRIME release result=%d\n",closeBo(fd,handle));
        closeSurfaceFd(prime);
    }
    printf("nm surface-bridge: diagnostic connection close=%d\n",closeDevice(fd));
}
