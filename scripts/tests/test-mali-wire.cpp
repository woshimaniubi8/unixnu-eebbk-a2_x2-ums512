// SPDX-License-Identifier: GPL-2.0-only
#include "NativeMaliABI.h"
#include "mali_native_wire.h"
#define SAME(a,b) static_assert(sizeof(a)==sizeof(b),#a " size")
#define FIELD(a,f,b,g) static_assert(offsetof(a,f)==offsetof(b,g),#a "::" #f)
SAME(nm_query,NativeMaliAbiQuery);
SAME(nm_alloc,NativeMaliAbiBoAlloc);
SAME(nm_handle,NativeMaliAbiBoAllocReply);
SAME(nm_map,NativeMaliAbiBoMap);
SAME(nm_va,NativeMaliAbiBoMapReply);
SAME(nm_cpu_map,NativeMaliAbiBoCpuMapReply);
SAME(nm_cpu_unmap,NativeMaliAbiBoCpuUnmap);
SAME(nm_submit,NativeMaliAbiSubmit);
SAME(nm_wait,NativeMaliAbiWaitFence);
SAME(nm_param,NativeMaliAbiParam);
SAME(nm_cache,NativeMaliAbiBoCacheSync);
SAME(nm_import_user,NativeMaliAbiBoImportUser);
SAME(nm_handle,NativeMaliAbiBoWaitShared);
FIELD(nm_query,max_bytes,NativeMaliAbiQuery,max_bo_bytes);
FIELD(nm_map,permissions,NativeMaliAbiBoMap,permissions);
FIELD(nm_cpu_map,bytes,NativeMaliAbiBoCpuMapReply,bytes);
FIELD(nm_submit,flags,NativeMaliAbiSubmit,flags);
FIELD(nm_param,value,NativeMaliAbiParam,value);
FIELD(nm_cache,type,NativeMaliAbiBoCacheSync,type);
FIELD(nm_import_user,flags,NativeMaliAbiBoImportUser,flags);
static_assert(NM_PARAM==NativeMaliMethodGetParam && NM_CACHE_SYNC==NativeMaliMethodBoCacheSync);
static_assert(NM_IMPORT_USER==NativeMaliMethodBoImportUser && NM_WAIT_SHARED==NativeMaliMethodBoWaitShared);
int main() {return 0;}
