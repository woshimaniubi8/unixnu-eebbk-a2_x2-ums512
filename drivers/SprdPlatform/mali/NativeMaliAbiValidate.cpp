/* SPDX-License-Identifier: GPL-2.0-only */
/* The checks themselves; the rules and the return-code split are in the header. */
#include "NativeMaliAbiValidate.h"

namespace {

constexpr uint64_t kPage = NativeMaliAbiPageBytes;
/* NativeMaliVmMaxIas: the address space NativeMaliVm builds for. */
constexpr uint64_t kVaSize = UINT64_C(1) << 48;

} // namespace

IOReturn NativeMaliAbiValidate::version(uint32_t abiVersion)
{
    /* v1 clients keep working: the reply echoes the version they asked for,
     * and GetDeviceInfo accepts both record sizes. */
    return (abiVersion == NativeMaliAbiVersion ||
        abiVersion == NativeMaliAbiVersionV1) ? kIOReturnSuccess :
        kIOReturnUnsupported;
}

IOReturn NativeMaliAbiValidate::method(uint32_t selector)
{
    if(selector < NativeMaliMethodQuery || selector > NativeMaliMethodLast)
        return kIOReturnUnsupported;
    return kIOReturnSuccess;
}

IOReturn NativeMaliAbiValidate::query(const NativeMaliAbiQuery &query)
{
    const IOReturn result = version(query.abi_version);
    if(result != kIOReturnSuccess)return result;
    if(query.flags != 0)return kIOReturnUnsupported;
    return kIOReturnSuccess;
}

IOReturn NativeMaliAbiValidate::boAlloc(const NativeMaliAbiBoAlloc &alloc,
    uint64_t maxBytes)
{
    if(alloc.bytes == 0 || (alloc.bytes % kPage) != 0)
        return kIOReturnBadArgument;
    if(maxBytes != 0 && alloc.bytes > maxBytes)return kIOReturnNoResources;
    return kIOReturnSuccess;
}

IOReturn NativeMaliAbiValidate::boFree(const NativeMaliAbiBoFree &free)
{
    return free.handle == 0 ? kIOReturnBadArgument : kIOReturnSuccess;
}

IOReturn NativeMaliAbiValidate::boMap(const NativeMaliAbiBoMap &map,
    uint64_t boBytes)
{
    if(map.handle == 0 || map.reserved != 0)return kIOReturnBadArgument;
    if(map.iova == 0 || (map.iova % kPage) != 0)return kIOReturnBadArgument;
    /* Read is always part of the mapping (panfrost maps IOMMU_READ |
     * IOMMU_WRITE | IOMMU_CACHE); write is the only optional bit. */
    if((map.permissions & NativeMaliBoRead) == 0)return kIOReturnBadArgument;
    if((map.permissions & ~static_cast<uint32_t>(NativeMaliBoRead |
        NativeMaliBoWrite)) != 0)return kIOReturnUnsupported;
    if(boBytes == 0 || (boBytes % kPage) != 0)return kIOReturnBadArgument;
    if(map.iova >= kVaSize || boBytes > kVaSize - map.iova)
        return kIOReturnBadArgument;
    return kIOReturnSuccess;
}

IOReturn NativeMaliAbiValidate::boCpuMap(const NativeMaliAbiBoCpuMap &map)
{
    return map.handle == 0 ? kIOReturnBadArgument : kIOReturnSuccess;
}

IOReturn NativeMaliAbiValidate::boCpuUnmap(const NativeMaliAbiBoCpuUnmap &unmap)
{
    if(unmap.handle == 0 || unmap.reserved != 0)return kIOReturnBadArgument;
    return kIOReturnSuccess;
}

IOReturn NativeMaliAbiValidate::submit(const NativeMaliAbiSubmit &submit)
{
    if(submit.flags != 0)return kIOReturnUnsupported;
    if(submit.reserved != 0)return kIOReturnBadArgument;
    if(submit.job_chain == 0)return kIOReturnBadArgument;
    if((submit.job_chain % NativeMaliAbiJobChainAlign) != 0)
        return kIOReturnBadArgument;
    if(submit.wait_fence == 0) {
        /* Nothing to wait for, so a timeout would be a misunderstanding. */
        if(submit.wait_timeout_ns != 0)return kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    /* A fence this driver handed out always has a seqno: submit() gives the
     * first job on a slot 1 (panfrost_job.c:106). */
    if(fenceSeqno(submit.wait_fence) == 0)return kIOReturnBadArgument;
    return kIOReturnSuccess;
}

IOReturn NativeMaliAbiValidate::waitFence(const NativeMaliAbiWaitFence &wait)
{
    if(wait.fence == 0 || fenceSeqno(wait.fence) == 0)
        return kIOReturnBadArgument;
    return kIOReturnSuccess;
}

uint64_t NativeMaliAbiValidate::packFence(uint32_t jobSlot, uint32_t seqno)
{
    return (static_cast<uint64_t>(jobSlot) << 32) | seqno;
}

uint32_t NativeMaliAbiValidate::fenceSlot(uint64_t fence)
{
    return static_cast<uint32_t>(fence >> 32);
}

uint32_t NativeMaliAbiValidate::fenceSeqno(uint64_t fence)
{
    return static_cast<uint32_t>(fence & 0xFFFFFFFFu);
}
