/* SPDX-License-Identifier: MIT */
/* Exercise shipping descriptor lifetime without storage or GPU simulation. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
struct surface {unsigned refs;size_t bytes,planes;uintptr_t base,first;};
typedef struct surface *IOSurfaceRef;
static struct surface actual={0,32768,0,0x100000000,0x100000000};
static IOSurfaceRef IOSurfaceLookup(uint32_t id){if(id!=7)return NULL;++actual.refs;return &actual;}
static void CFRelease(IOSurfaceRef surface){assert(surface==&actual && surface->refs);--surface->refs;}
static size_t IOSurfaceGetPlaneCount(IOSurfaceRef s){return s->planes;}
static size_t IOSurfaceGetAllocSize(IOSurfaceRef s){return s->bytes;}
static void *IOSurfaceGetBaseAddress(IOSurfaceRef s){return (void *)s->base;}
static void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s,unsigned plane){(void)plane;return (void *)s->first;}
#include "native_mali_surface_fd.inc"
int main(void)
{
    assert(setenv("TMPDIR","/this/path/does/not/exist",1)==0);
    uint64_t size=0;
    assert(vmg_drm_surface_fd(1,8,0)==-1 && errno==ENOENT && actual.refs==0);
    assert(vmg_drm_surface_fd(1,7,1)==-1 && errno==EINVAL && actual.refs==0);
    actual.planes=2;actual.first=actual.base+4096;
    int fd=vmg_drm_surface_fd(1,7,1);assert(fd>=0 && actual.refs==1);
    assert(vmg_drm_surface_size(fd,&size)==0 && size==28672);
    assert(lseek(fd,0,SEEK_END)==0); /* actual fd stores no surface-sized file */
    vmg_drm_surface_fd_close(fd);assert(actual.refs==0);
    assert(vmg_drm_surface_size(fd,&size)==-EBADF && size==0);
    vmg_drm_surface_fd_close(fd);assert(actual.refs==0);
    actual.planes=0;actual.first=actual.base;
    int fds[256];
    for(unsigned i=0;i<256;i++){fds[i]=vmg_drm_surface_fd(1,7,0);assert(fds[i]>=0);}
    assert(actual.refs==256);
    assert(vmg_drm_surface_fd(1,7,0)==-1 && errno==ENOSPC && actual.refs==256);
    for(unsigned i=0;i<256;i++)vmg_drm_surface_fd_close(fds[i]);
    assert(actual.refs==0);
    int save=dup(STDIN_FILENO);assert(save>=0);assert(close(STDIN_FILENO)==0);
    fd=vmg_drm_surface_fd(1,7,0);assert(fd==0 && actual.refs==1);
    assert(vmg_drm_surface_size(fd,&size)==0 && size==32768);
    vmg_drm_surface_fd_close(fd);assert(actual.refs==0);
    assert(dup2(save,STDIN_FILENO)==STDIN_FILENO);close(save);
    fd=vmg_drm_bo_fd(1,42);assert(fd>=0);
    assert(vmg_drm_surface_size(fd,&size)==-EINVAL);vmg_drm_surface_fd_close(fd);
    puts("PASS: storage-free original surface metadata, plane size, fd0, bounded registry and balanced lifetime; no pixel or GPU execution asserted.");
    return 0;
}
