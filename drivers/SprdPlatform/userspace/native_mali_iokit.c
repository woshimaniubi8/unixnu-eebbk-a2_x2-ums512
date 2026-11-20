/* SPDX-License-Identifier: MIT */
#include "mali_native_wire.h"
#include "iokit_compat.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <IOSurface/IOSurface.h>
extern CFMutableDictionaryRef IOServiceNameMatching(const char *);
struct iokit_connection { io_connect_t port; };
static int error(kern_return_t kr)
{
    switch(kr){
    case kIOReturnSuccess:return 0;
    case kIOReturnNoMemory:return -ENOMEM;
    case kIOReturnNoResources:return -ENOSPC;
    case kIOReturnUnsupported:return -ENOTSUP;
    case kIOReturnNotFound:return -ENOENT;
    case kIOReturnNotPrivileged:case kIOReturnNotPermitted:return -EACCES;
    case kIOReturnBadArgument:return -EINVAL;
    case kIOReturnTimeout:return -ETIMEDOUT;
    case kIOReturnBusy:return -EBUSY;
    default:return -EIO;
    }
}
static int invoke(void *context,uint32_t selector,const void *input,size_t ni,void *output,size_t no)
{
    struct iokit_connection *c=context;size_t actual=no;
    int r=error(IOConnectCallStructMethod(c->port,selector,input,ni,output,&actual));
    return r?r:(actual==no?0:-EPROTO);
}
static int map(void *context,uint32_t token,void **p,size_t *bytes)
{
    struct iokit_connection *c=context;mach_vm_address_t address=0;mach_vm_size_t size=*bytes;
    int r=error(IOConnectMapMemory64(c->port,token,mach_task_self(),&address,&size,kIOMapAnywhere));
    if(!r){*p=(void *)(uintptr_t)address;*bytes=(size_t)size;}return r;
}
static int unmap(void *context,uint32_t token,void *p)
{ return error(IOConnectUnmapMemory64(((struct iokit_connection *)context)->port,token,mach_task_self(),(uintptr_t)p)); }
static void close_connection(void *context)
{
    struct iokit_connection *c=context;IOServiceClose(c->port);free(c);
}
static uint64_t now_ns(void *unused)
{
    (void)unused;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static void release_surface(void *surface){if(surface)CFRelease(surface);}
static int surface_view(void *context,uint32_t id,uint32_t plane,struct nm_surface_view *view)
{
    (void)context;memset(view,0,sizeof(*view));
    IOSurfaceRef surface=IOSurfaceLookup(id);
    if(!surface)return -ENOENT;
    size_t planes=IOSurfaceGetPlaneCount(surface),total=IOSurfaceGetAllocSize(surface);
    if((planes?plane>=planes:plane!=0) || !total){CFRelease(surface);return -EINVAL;}
    uintptr_t base=(uintptr_t)IOSurfaceGetBaseAddress(surface);
    uintptr_t first=planes?(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,plane):base;
    size_t width=planes?IOSurfaceGetWidthOfPlane(surface,plane):IOSurfaceGetWidth(surface);
    size_t height=planes?IOSurfaceGetHeightOfPlane(surface,plane):IOSurfaceGetHeight(surface);
    size_t row=planes?IOSurfaceGetBytesPerRowOfPlane(surface,plane):IOSurfaceGetBytesPerRow(surface);
    const uint64_t page=16384;
    if(!base || (base&(page-1)) || total>UINTPTR_MAX-base || first<base || first-base>=total ||
        total>UINT64_MAX-(page-1) || !width || !height || !row ||
        width>UINT32_MAX || height>UINT32_MAX || row>UINT32_MAX ||
        row>UINT64_MAX/height || row*height>total-(first-base)){
        CFRelease(surface);return -EINVAL;
    }
    uint64_t start=(first-base)&~(page-1),end=(total+page-1)&~(page-1);
    *view=(struct nm_surface_view){.address=base+start,.bytes=end-start,
        .in_page=first-base-start,.plane_bytes=total-(first-base),
        .bytes_per_row=(uint32_t)row,.pixel_format=IOSurfaceGetPixelFormat(surface),
        .width=(uint32_t)width,.height=(uint32_t)height,.retained=(void *)surface};
    return 0;
}
int nm_iokit_open(void **output)
{
    if(!output)return -EINVAL;*output=NULL;
    io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceNameMatching("NativeMaliGpuService"));
    if(!service)return -ENODEV;
    struct iokit_connection *c=calloc(1,sizeof(*c));if(!c){IOObjectRelease(service);return -ENOMEM;}
    int r=error(IOServiceOpen(service,mach_task_self(),0,&c->port));IOObjectRelease(service);
    if(r){free(c);return r;}
    static const struct nm_transport ops={invoke,map,unmap,close_connection,now_ns,16384,surface_view,release_surface};
    /* iOS arm64's CPU page size is queried rather than inferred from GPU PTEs. */
    long page=sysconf(_SC_PAGESIZE);if(page!=16384){close_connection(c);return -ENOTSUP;}
    return nm_backend_connect(&ops,c,output);
}
