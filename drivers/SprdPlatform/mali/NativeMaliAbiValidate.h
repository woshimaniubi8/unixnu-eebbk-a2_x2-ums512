/* SPDX-License-Identifier: GPL-2.0-only */
/* The part of the user-client checks that one ABI record decides on its own.
 *
 * Everything here is pure: no registers, no IOKit objects, no client state. What
 * needs the client's bookkeeping - does this handle exist, is this fence this
 * client's, is this IOVA already taken, is the job slot free - is not here,
 * because a pure function cannot know it. Keeping the mechanical checks in one
 * place means the IOUserClient glue only has to look up its own tables and then
 * call these.
 *
 * A malformed request is kIOReturnBadArgument; an unknown version, method or
 * flag bit is kIOReturnUnsupported (a newer client, not a broken one); a
 * request that is well formed but cannot be satisfied is kIOReturnNoResources.
 */
#ifndef NATIVE_MALI_ABI_VALIDATE_H
#define NATIVE_MALI_ABI_VALIDATE_H
#include "NativeMaliABI.h"
#include <IOKit/IOTypes.h>
#include <stdint.h>

class NativeMaliAbiValidate final {
public:
    NativeMaliAbiValidate() = delete;

    static IOReturn version(uint32_t abiVersion);
    static IOReturn method(uint32_t selector);

    /* Query: the version must match and no flag bit is defined yet. */
    static IOReturn query(const NativeMaliAbiQuery &query);

    /* nonzero, a page multiple, and no bigger than `maxBytes` (the service
     * passes what Query advertises). */
    static IOReturn boAlloc(const NativeMaliAbiBoAlloc &alloc, uint64_t maxBytes);
    static IOReturn boFree(const NativeMaliAbiBoFree &free);
    /* `boBytes` is the size of the buffer the handle names; the map record
     * carries no size of its own. The range must stay inside the 48-bit
     * address space NativeMaliVm builds for. */
    static IOReturn boMap(const NativeMaliAbiBoMap &map, uint64_t boBytes);
    static IOReturn boCpuMap(const NativeMaliAbiBoCpuMap &map);
    static IOReturn boCpuUnmap(const NativeMaliAbiBoCpuUnmap &unmap);

    /* The chain must be where the hardware expects a job header, and a wait
     * with no fence cannot have a timeout. */
    static IOReturn submit(const NativeMaliAbiSubmit &submit);
    static IOReturn waitFence(const NativeMaliAbiWaitFence &wait);

    /* Fence encoding: the low half is the job slot's emit seqno (which starts
     * at 1, so a fence is never 0), the high half the slot. Opaque to
     * userspace; the service round-trips it. */
    static uint64_t packFence(uint32_t jobSlot, uint32_t seqno);
    static uint32_t fenceSlot(uint64_t fence);
    static uint32_t fenceSeqno(uint64_t fence);
};

#endif
