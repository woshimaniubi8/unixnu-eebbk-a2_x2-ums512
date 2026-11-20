/* SPDX-License-Identifier: MIT */
/* Independently declared NativeMali wire layouts. These are byte records, not
 * kernel C++ objects. scripts/tests/test-mali-wire.cpp checks both declarations. */
#pragma once
#include <stddef.h>
#include <stdint.h>
enum { NM_QUERY=1, NM_INFO=2, NM_ALLOC=3, NM_MAP=4, NM_FREE=5,
       NM_CPU_MAP=6, NM_CPU_UNMAP=7, NM_SUBMIT=8, NM_WAIT=9,
       NM_STATUS=10, NM_PARAM=12, NM_CACHE_SYNC=13 };
struct nm_query { uint32_t version,ias,oas,flags; uint64_t max_bytes; };
struct nm_alloc { uint64_t bytes; };
struct nm_handle { uint64_t handle; };
struct nm_map { uint64_t handle,va; uint32_t permissions,reserved; };
struct nm_va { uint64_t va; };
struct nm_cpu_map { uint32_t token,reserved; uint64_t bytes; };
struct nm_cpu_unmap { uint64_t handle; uint32_t token,reserved; };
struct nm_submit { uint64_t chain,dependency,timeout; uint32_t flags,reserved; };
struct nm_fence_wire { uint64_t fence; };
struct nm_wait { uint64_t fence,timeout; };
struct nm_param { uint32_t parameter,reserved; uint64_t value; };
struct nm_cache { uint64_t handle,offset,bytes; uint32_t type,reserved; };

/* Transport returns 0 or negative errno, and must reject short/long replies. */
struct nm_transport {
    int (*call)(void *,uint32_t,const void *,size_t,void *,size_t);
    int (*map)(void *,uint32_t,void **,size_t *);
    int (*unmap)(void *,uint32_t,void *);
    void (*close)(void *);
    uint64_t (*now_ns)(void *);
    uint32_t page_size;
};
int nm_backend_connect(const struct nm_transport *,void *,void **);
