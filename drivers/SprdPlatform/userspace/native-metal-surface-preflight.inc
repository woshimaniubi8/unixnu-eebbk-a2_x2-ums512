/* SPDX-License-Identifier: MIT */
/* Original-surface allocation and native pinning, before loading Mesa. */
#include "mali_native_wire.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>

static int surfaceTempFile(const char *directory)
{
    char name[1024];
    int n=snprintf(name,sizeof(name),"%s/vgpu-check-XXXXXX",directory);
    if(n<0 || (size_t)n>=sizeof(name))return ENAMETOOLONG;
    int fd=mkstemp(name);
    if(fd<0)return errno;
    int result=0;
    if(unlink(name)!=0)result=errno;
    if(!result && ftruncate(fd,32768)!=0)result=errno;
    if(!result && lseek(fd,0,SEEK_END)!=32768)result=errno?errno:EIO;
    if(close(fd)!=0 && !result)result=errno;
    return result;
}

static int originalSurfacePreflight(void)
{
    const char *previous=getenv("TMPDIR");
    printf("nm surface-preflight: previous tmp=%s file_errno=%d\n",
        previous?previous:"/tmp",surfaceTempFile(previous?previous:"/tmp"));
    const char *directory="/private/var/ngpu/mali-metal-tmp";
    if(mkdir(directory,0700)!=0 && errno!=EEXIST){
        fprintf(stderr,"nm surface-preflight: private mkdir errno=%d\n",errno);return 71;
    }
    struct stat status={0};
    if(lstat(directory,&status)!=0 || !S_ISDIR(status.st_mode) || status.st_uid!=getuid() ||
        (status.st_mode&077)!=0 || setenv("TMPDIR",directory,1)!=0){
        fprintf(stderr,"nm surface-preflight: private temporary-directory contract failed errno=%d\n",errno);return 72;
    }
    int temporary=surfaceTempFile(directory);
    printf("nm surface-preflight: configured tmp=%s file_errno=%d\n",directory,temporary);
    if(temporary)return 73;
    NSDictionary *properties=@{(__bridge id)kIOSurfaceWidth:@64,(__bridge id)kIOSurfaceHeight:@64,
        (__bridge id)kIOSurfaceBytesPerElement:@4,(__bridge id)kIOSurfaceBytesPerRow:@256,
        (__bridge id)kIOSurfacePixelFormat:@0x52474241,(__bridge id)kIOSurfaceAllocSize:@32768};
    IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if(!surface){fprintf(stderr,"nm surface-preflight: original create failed\n");return 74;}
    void *base=IOSurfaceGetBaseAddress(surface);size_t bytes=IOSurfaceGetAllocSize(surface);
    printf("nm surface-preflight: original id=%u base=0x%llx bytes=0x%llx page=%ld\n",
        IOSurfaceGetID(surface),(unsigned long long)(uintptr_t)base,(unsigned long long)bytes,sysconf(_SC_PAGESIZE));
    if(!base || bytes<32768 || IOSurfaceLock(surface,0,NULL)!=KERN_SUCCESS){CFRelease(surface);return 75;}
    memset(base,0x6b,bytes);IOSurfaceUnlock(surface,0,NULL);
    void *image=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
    CFMutableDictionaryRef (*matching)(const char *)=image?dlsym(image,"IOServiceNameMatching"):NULL;
    mach_port_t (*getService)(mach_port_t,CFDictionaryRef)=image?dlsym(image,"IOServiceGetMatchingService"):NULL;
    kern_return_t (*openService)(mach_port_t,mach_port_t,uint32_t,mach_port_t *)=image?dlsym(image,"IOServiceOpen"):NULL;
    kern_return_t (*objectRelease)(mach_port_t)=image?dlsym(image,"IOObjectRelease"):NULL;
    kern_return_t (*closeService)(mach_port_t)=image?dlsym(image,"IOServiceClose"):NULL;
    kern_return_t (*call)(mach_port_t,uint32_t,const void *,size_t,void *,size_t *)=image?dlsym(image,"IOConnectCallStructMethod"):NULL;
    if(!matching || !getService || !openService || !objectRelease || !closeService || !call){
        CFRelease(surface);if(image)dlclose(image);return 76;
    }
    mach_port_t service=getService(0,matching("NativeMaliGpuService")),connection=MACH_PORT_NULL;
    kern_return_t result=service?openService(service,mach_task_self(),0,&connection):KERN_FAILURE;
    if(service)objectRelease(service);
    if(result!=KERN_SUCCESS){fprintf(stderr,"nm surface-preflight: original native open kr=0x%x\n",result);CFRelease(surface);dlclose(image);return 77;}
    struct nm_import_user request={(uintptr_t)base,bytes,0,0};struct nm_handle handle={0};size_t length=sizeof(handle);
    result=call(connection,NM_IMPORT_USER,&request,sizeof(request),&handle,&length);
    printf("nm surface-preflight: native14 kr=0x%x reply=%lu handle=0x%llx\n",
        result,(unsigned long)length,(unsigned long long)handle.handle);
    kern_return_t freed=KERN_SUCCESS;
    if(result==KERN_SUCCESS && handle.handle){
        size_t empty=0;freed=call(connection,NM_FREE,&handle,sizeof(handle),NULL,&empty);
        printf("nm surface-preflight: native5 release kr=0x%x\n",freed);
    }
    kern_return_t closed=closeService(connection);dlclose(image);
    int valid=IOSurfaceLock(surface,kIOSurfaceLockReadOnly,NULL)==KERN_SUCCESS;
    if(valid){
        for(size_t i=0;i<bytes;i++)if(((const uint8_t *)base)[i]!=0x6b){valid=0;break;}
        IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL);
    }
    CFRelease(surface);
    if(result!=KERN_SUCCESS || length!=sizeof(handle) || !handle.handle ||
        freed!=KERN_SUCCESS || closed!=KERN_SUCCESS || !valid)return 78;
    printf("nm surface-preflight: PASS original_pages=1 unchanged=1 released=1\n");
    return 0;
}
