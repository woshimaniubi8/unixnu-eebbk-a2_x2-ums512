/* SPDX-License-Identifier: MIT */
/* A PRIME shim fd identifies retained original pages; it stores no pixel data. */
typedef struct {
    int fd;
    uint32_t surface,plane;
    bool is_bo;
    uint32_t bo_handle;
    uint64_t bytes;
    IOSurfaceRef retained;
    bool used;
} SurfaceFd;
static SurfaceFd g_surf[256];
static pthread_mutex_t g_surf_lock=PTHREAD_MUTEX_INITIALIZER;

static int surface_fd_register(SurfaceFd entry)
{
    int fd=open("/dev/null",O_RDONLY|O_CLOEXEC);
    if(fd<0){if(entry.retained)CFRelease(entry.retained);return -1;}
    entry.fd=fd;entry.used=true;
    pthread_mutex_lock(&g_surf_lock);
    unsigned slot=0;
    while(slot<256 && g_surf[slot].used)++slot;
    if(slot<256)g_surf[slot]=entry;
    pthread_mutex_unlock(&g_surf_lock);
    if(slot==256){
        close(fd);if(entry.retained)CFRelease(entry.retained);errno=ENOSPC;return -1;
    }
    return fd;
}

int vmg_drm_surface_fd(int device_fd,uint32_t surface_id,unsigned plane)
{
    (void)device_fd;
    IOSurfaceRef surface=IOSurfaceLookup(surface_id);
    if(!surface){errno=ENOENT;return -1;}
    size_t planes=IOSurfaceGetPlaneCount(surface),total=IOSurfaceGetAllocSize(surface);
    if((planes?plane>=planes:plane!=0) || !total){CFRelease(surface);errno=EINVAL;return -1;}
    uintptr_t base=(uintptr_t)IOSurfaceGetBaseAddress(surface);
    uintptr_t first=planes?(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,plane):base;
    if(!base || !first ||
        total>UINTPTR_MAX-base || first<base || first-base>=total){
        CFRelease(surface);errno=EINVAL;return -1;
    }
    return surface_fd_register((SurfaceFd){.surface=surface_id,.plane=plane,
        .bytes=total-(first-base),.retained=surface});
}

int vmg_drm_bo_fd(int device_fd,uint32_t handle)
{
    (void)device_fd;
    if(!handle){errno=EINVAL;return -1;}
    return surface_fd_register((SurfaceFd){.is_bo=true,.bo_handle=handle});
}

int vmg_drm_surface_size(int fd,uint64_t *bytes)
{
    if(!bytes)return -EINVAL;
    *bytes=0;int result=-EBADF;
    pthread_mutex_lock(&g_surf_lock);
    for(unsigned i=0;i<256;i++)if(g_surf[i].used && g_surf[i].fd==fd){
        *bytes=g_surf[i].bytes;result=*bytes?0:-EINVAL;break;
    }
    pthread_mutex_unlock(&g_surf_lock);
    return result;
}

void vmg_drm_surface_fd_close(int fd)
{
    SurfaceFd entry={0};
    pthread_mutex_lock(&g_surf_lock);
    for(unsigned i=0;i<256;i++)if(g_surf[i].used && g_surf[i].fd==fd){
        entry=g_surf[i];g_surf[i]=(SurfaceFd){0};break;
    }
    pthread_mutex_unlock(&g_surf_lock);
    if(entry.used){close(entry.fd);if(entry.retained)CFRelease(entry.retained);}
}
