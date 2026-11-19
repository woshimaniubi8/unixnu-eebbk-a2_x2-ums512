/* SPDX-License-Identifier: GPL-2.0-only */
/* Buffer objects; the sources for every decision here are in NativeMaliMemory.h.
 */
#include "NativeMaliMemory.h"
#include <string.h>

namespace {

IOReturn checkBufferRange(size_t total, size_t offset, size_t bytes)
{
    if(offset > total)return kIOReturnBadArgument;
    if(bytes > total - offset)return kIOReturnBadArgument;
    return kIOReturnSuccess;
}

} // namespace

NativeMaliMemory::~NativeMaliMemory()
{
    /* No registers are touched here, so a still-mapped buffer is only dropped
     * from the CPU side. The caller's address-space teardown must already have
     * run; a stale IOVA is a GPU fault, not a leak. */
    reset();
}

IOReturn NativeMaliMemory::prepare(NativeMaliMemorySource *source, size_t bytes,
    bool cpuCoherent, NativeMaliCacheMaintainer *maintainer)
{
    if(prepared_)return kIOReturnBusy;
    if(!source || !bytes)return kIOReturnBadArgument;
    /* Only whole 4 KiB pages can be described by a leaf, and only whole pages
     * can be mapped. */
    if(bytes & (NativeMaliVmGranule - 1))return kIOReturnBadArgument;
    /* A non-coherent buffer without a maintainer could only be mapped by
     * silently skipping the clean/invalidate the hardware needs. */
    if(!cpuCoherent && !maintainer)return kIOReturnBadArgument;

    NativeMaliMemorySpan spans[NativeMaliMemoryMaxSpans]{};
    void *cpu = nullptr;
    size_t count = 0;
    IOReturn result = source->alloc(bytes, &cpu, spans, &count);
    if(result != kIOReturnSuccess)return result;

    /* The span list must describe the buffer exactly: page-aligned runs, in
     * offset order, with no gap and no overlap. Anything else is a backend bug
     * that would map the wrong physical page, so refuse and give the
     * allocation back. */
    IOReturn bad = kIOReturnSuccess;
    if(!cpu || count == 0 || count > NativeMaliMemoryMaxSpans)bad = kIOReturnBadArgument;
    uint64_t expected = 0;
    for(size_t i = 0; bad == kIOReturnSuccess && i < count; ++i) {
        const NativeMaliMemorySpan &span = spans[i];
        if(!span.bytes || span.offset != expected ||
            (span.offset & (NativeMaliVmGranule - 1)) ||
            (span.bytes & (NativeMaliVmGranule - 1)) ||
            (span.physical & (NativeMaliVmGranule - 1)) ||
            span.offset + span.bytes < span.offset ||
            span.physical + span.bytes < span.physical)
            bad = kIOReturnBadArgument;
        else
            expected += span.bytes;
    }
    if(bad == kIOReturnSuccess && expected != bytes)bad = kIOReturnBadArgument;
    if(bad != kIOReturnSuccess) {
        source->release(cpu, spans, count);
        return bad;
    }

    memcpy(span_, spans, count * sizeof(spans[0]));
    spans_ = count;
    cpu_ = cpu;
    bytes_ = bytes;
    coherent_ = cpuCoherent;
    maintainer_ = maintainer;
    source_ = source;
    prepared_ = true;
    return kIOReturnSuccess;
}

void NativeMaliMemory::reset()
{
    if(prepared_ && source_)source_->release(cpu_, span_, spans_);
    memset(span_, 0, sizeof(span_));
    spans_ = 0;
    source_ = nullptr;
    maintainer_ = nullptr;
    cpu_ = nullptr;
    bytes_ = 0;
    coherent_ = false;
    prepared_ = false;
    vm_ = nullptr;
    mmu_ = nullptr;
    as_ = 0;
    iova_ = 0;
    mapped_ = false;
}

IOReturn NativeMaliMemory::release()
{
    if(!prepared_)return kIOReturnNotReady;
    /* The leaves would outlive the pages they point at. unmap() first. */
    if(mapped_)return kIOReturnBusy;
    reset();
    return kIOReturnSuccess;
}

IOReturn NativeMaliMemory::map(NativeMaliVm *vm, NativeMaliMmu *mmu, uint32_t as,
    uint64_t iova, uint32_t prot)
{
    if(!prepared_)return kIOReturnNotReady;
    if(mapped_)return kIOReturnBusy;
    if(!vm || !mmu)return kIOReturnBadArgument;
    if(iova & (NativeMaliVmGranule - 1))return kIOReturnBadArgument;
    /* Check the address space before the first leaf is written: a bad slot
     * would otherwise leave mappings behind that no flush ever publishes. */
    if(as >= NativeMaliMmuMaxAs || (mmu->present() & (1u << as)) == 0 ||
        !mmu->isAllocated(as))
        return kIOReturnBadArgument;

    /* mmu_map_sg(), panfrost_mmu.c:421-464: one segment at a time, in IOVA
     * order, then one flush for the range. A failure unmaps what was already
     * mapped and returns the original error. */
    uint64_t done = 0;
    for(size_t i = 0; i < spans_; ++i) {
        const IOReturn result = vm->map(iova + span_[i].offset,
            span_[i].physical, span_[i].bytes, prot);
        if(result != kIOReturnSuccess) {
            if(done)(void)vm->unmap(iova, done);
            return result;
        }
        done += span_[i].bytes;
    }

    const IOReturn result = mmu->flushRange(as, iova, bytes_);
    if(result != kIOReturnSuccess) {
        (void)vm->unmap(iova, bytes_);
        return result;
    }

    vm_ = vm;
    mmu_ = mmu;
    as_ = as;
    iova_ = iova;
    mapped_ = true;
    return kIOReturnSuccess;
}

IOReturn NativeMaliMemory::unmap()
{
    if(!mapped_)return kIOReturnBadArgument;
    IOReturn result = vm_->unmap(iova_, bytes_);
    if(result != kIOReturnSuccess)return result;
    result = mmu_->flushRange(as_, iova_, bytes_);
    if(result != kIOReturnSuccess) {
        /* The leaves are clear but the GPU may still translate the IOVA. Keep
         * the mapping recorded so a retry repeats the flush; Vm::unmap() on an
         * already-clear range is a no-op. */
        return result;
    }
    mapped_ = false;
    vm_ = nullptr;
    mmu_ = nullptr;
    as_ = 0;
    iova_ = 0;
    return kIOReturnSuccess;
}

IOReturn NativeMaliMemory::cleanForDevice(size_t offset, size_t bytes)
{
    if(!prepared_)return kIOReturnNotReady;
    IOReturn result = checkBufferRange(bytes_, offset, bytes);
    if(result != kIOReturnSuccess)return result;
    /* dma_map_sgtable() is a no-op on a coherent device (panfrost_device.c:269
     * decides which one this is). */
    if(!bytes || coherent_)return kIOReturnSuccess;
    return maintainer_->maintain(static_cast<uint8_t *>(cpu_) + offset, bytes,
        NativeMaliCacheClean);
}

IOReturn NativeMaliMemory::invalidateForCpu(size_t offset, size_t bytes)
{
    if(!prepared_)return kIOReturnNotReady;
    IOReturn result = checkBufferRange(bytes_, offset, bytes);
    if(result != kIOReturnSuccess)return result;
    if(!bytes || coherent_)return kIOReturnSuccess;
    return maintainer_->maintain(static_cast<uint8_t *>(cpu_) + offset, bytes,
        NativeMaliCacheInvalidate);
}
