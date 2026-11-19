/* SPDX-License-Identifier: GPL-2.0-only */
/* UNVERIFIED(XNU): never compiled against XNU headers (only -fsyntax-only
 * against local stubs); see docs/UNVERIFIED.md.
 * The IOKit calls mirror NativeAdreno's in-tree implementations (MIT, NOTICE):
 * page allocation and span walking from NativeAdrenoMemory::allocate(), the
 * cache sequence from NativeAdrenoCpu.cpp, and the register window from
 * NativeAdrenoGpuService. */
#include "NativeMaliXnuSupport.h"
#include <kern/cpu_data.h>
#include <kern/task.h>
#include <mach/vm_param.h>
#include <string.h>

namespace {

uint64_t outputLimit(uint32_t bits)
{
    if(bits >= 32 && bits <= NativeMaliVmMaxOas)return UINT64_C(1) << bits;
    /* A probe that did not answer leaves oas at 0; the Mali maximum is the
     * conservative ceiling then. */
    return UINT64_C(1) << NativeMaliVmMaxOas;
}

IOOptionBits bufferOptions(bool contiguous)
{
    IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
        static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(contiguous)options |= static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    return options;
}

} // namespace

void NativeMaliXnuPages::setOutputLimit(uint32_t oas_bits)
{
    limit_ = outputLimit(oas_bits);
}

IOReturn NativeMaliXnuWindow::read32(uint32_t offset, uint32_t *value)
{
    if(!value || !base_ || (offset & 3u) || offset > bytes_ || bytes_ - offset < 4)
        return kIOReturnBadArgument;
    *value = *reinterpret_cast<volatile uint32_t *>(base_ + offset);
    return kIOReturnSuccess;
}

IOReturn NativeMaliXnuWindow::write32(uint32_t offset, uint32_t value)
{
    if(!base_ || (offset & 3u) || offset > bytes_ || bytes_ - offset < 4)
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint32_t *>(base_ + offset) = value;
    __asm__ volatile ("dsb sy" ::: "memory");
    return kIOReturnSuccess;
}

IOReturn NativeMaliXnuWindow::delay(uint64_t microseconds)
{
    if(microseconds > UINT32_MAX)return kIOReturnBadArgument;
    /* UNVERIFIED(XNU): IODelay is a spin, which is what a 10 us hardware poll
     * wants; whether it is allowed under this lock and really waits that long
     * has not been checked (docs/UNVERIFIED.md). */
    IODelay(static_cast<UInt32>(microseconds));
    return kIOReturnSuccess;
}

IOReturn NativeMaliXnuPages::alloc(NativeMaliPage *page)
{
    if(!page)return kIOReturnBadArgument;
    Entry *slot = nullptr;
    for(auto &entry : entry_)
        if(!entry.descriptor) { slot = &entry; break; }
    if(!slot)return kIOReturnNoResources;

    IOBufferMemoryDescriptor *descriptor =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            bufferOptions(true), PAGE_SIZE,
            (limit_ - 1) & ~static_cast<uint64_t>(PAGE_MASK));
    if(descriptor) {
        memset(descriptor->getBytesNoCopy(),0,descriptor->getLength());
        descriptor->setLength(NativeMaliVmGranule);
    }
    if(!descriptor)return kIOReturnNoMemory;
    IOReturn status = descriptor->prepare();
    if(status != kIOReturnSuccess) {
        descriptor->release();
        return status;
    }

    IOByteCount length = 0;
    void *cpu = descriptor->getBytesNoCopy();
    const addr64_t physical =
        descriptor->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    if(!cpu || (reinterpret_cast<uintptr_t>(cpu) & (NativeMaliVmGranule - 1)) ||
        !physical || (physical & (NativeMaliVmGranule - 1)) ||
        length < NativeMaliVmGranule || physical >= limit_ ||
        NativeMaliVmGranule > limit_ - physical ||
        descriptor->getLength() != NativeMaliVmGranule) {
        (void)descriptor->complete();
        descriptor->release();
        return kIOReturnUnsupported;
    }

    memset(cpu, 0, NativeMaliVmGranule);
    slot->cpu = cpu;
    slot->descriptor = descriptor;
    page->cpu = cpu;
    page->phys = physical;
    page->bytes = NativeMaliVmGranule;
    return kIOReturnSuccess;
}

void NativeMaliXnuPages::release(const NativeMaliPage &page)
{
    for(auto &entry : entry_) {
        if(entry.cpu != page.cpu || !entry.descriptor)continue;
        IOBufferMemoryDescriptor *descriptor = entry.descriptor;
        entry = Entry{};
        const IOReturn status = descriptor->complete();
        if(status != kIOReturnSuccess) {
            /* complete() failed: the pages are still wired, so releasing them
             * would free memory the GPU may still walk. Leak the descriptor on
             * purpose; docs/UNVERIFIED.md records it. */
            IOLog("NativeMali: page-table complete failed 0x%x, leaking one page\n",
                status);
            return;
        }
        descriptor->release();
        return;
    }
}

size_t NativeMaliXnuPages::live() const
{
    size_t count = 0;
    for(const auto &entry : entry_)
        if(entry.descriptor)++count;
    return count;
}

void NativeMaliXnuBuffers::setOutputLimit(uint32_t oas_bits)
{
    limit_ = outputLimit(oas_bits);
}

IOReturn NativeMaliXnuBuffers::alloc(size_t bytes, void **cpu,
    NativeMaliMemorySpan *spans, size_t *count)
{
    if(!cpu || !spans || !count)return kIOReturnBadArgument;
    if(bytes == 0 || (bytes % NativeMaliVmGranule) != 0 || bytes > limit_)
        return kIOReturnBadArgument;

    Entry *slot = nullptr;
    for(auto &entry : entry_)
        if(!entry.descriptor) { slot = &entry; break; }
    if(!slot)return kIOReturnNoResources;

    // Allocate host pages, expose only the ABI's requested 4K-multiple length.
    // The GPU maps physical spans using its own 4K granule on this 16K XNU.
    if(bytes>SIZE_MAX-PAGE_MASK)return kIOReturnBadArgument;
    const size_t hostBytes=(bytes+PAGE_MASK)&~static_cast<size_t>(PAGE_MASK);
    IOBufferMemoryDescriptor *descriptor =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            bufferOptions(false),hostBytes,
            (limit_-1)&~static_cast<uint64_t>(PAGE_MASK));
    if(descriptor) {
        memset(descriptor->getBytesNoCopy(),0,descriptor->getLength());
        descriptor->setLength(bytes);
    }
    if(!descriptor)return kIOReturnNoMemory;
    IOReturn status = descriptor->prepare();
    if(status != kIOReturnSuccess) {
        descriptor->release();
        return status;
    }

    void *mapping = descriptor->getBytesNoCopy();
    size_t spans_used = 0;
    status = kIOReturnUnsupported;
    if(!mapping || (reinterpret_cast<uintptr_t>(mapping) & (NativeMaliVmGranule - 1)) ||
        descriptor->getLength() != bytes)
        goto failed;

    for(size_t offset = 0; offset < bytes; offset += NativeMaliVmGranule) {
        IOByteCount length = 0;
        const addr64_t physical =
            descriptor->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);
        if(!physical || (physical & (NativeMaliVmGranule - 1)) ||
            length < NativeMaliVmGranule || physical >= limit_ ||
            NativeMaliVmGranule > limit_ - physical)
            goto failed;
        if(spans_used &&
            physical == spans[spans_used - 1].physical + spans[spans_used - 1].bytes) {
            spans[spans_used - 1].bytes += NativeMaliVmGranule;
        } else {
            if(spans_used == NativeMaliMemoryMaxSpans)goto failed;
            spans[spans_used].offset = offset;
            spans[spans_used].physical = physical;
            spans[spans_used].bytes = NativeMaliVmGranule;
            ++spans_used;
        }
    }

    memset(mapping, 0, bytes);
    slot->cpu = mapping;
    slot->descriptor = descriptor;
    slot->bytes = bytes;
    *cpu = mapping;
    *count = spans_used;
    return kIOReturnSuccess;

failed:
    (void)descriptor->complete();
    descriptor->release();
    return status;
}

void NativeMaliXnuBuffers::release(void *cpu, const NativeMaliMemorySpan *,
    size_t)
{
    if(!cpu)return;
    for(auto &entry : entry_) {
        if(entry.cpu != cpu || !entry.descriptor)continue;
        IOBufferMemoryDescriptor *descriptor = entry.descriptor;
        entry = Entry{};
        const IOReturn status = descriptor->complete();
        if(status != kIOReturnSuccess) {
            IOLog("NativeMali: buffer complete failed 0x%x, leaking %lu bytes\n",
                status, static_cast<unsigned long>(descriptor->getLength()));
            return;
        }
        descriptor->release();
        return;
    }
}

IOMemoryDescriptor *NativeMaliXnuBuffers::descriptor(void *cpu)
{
    if(!cpu)return nullptr;
    for(auto &entry : entry_)
        if(entry.cpu == cpu && entry.descriptor) {
            entry.descriptor->retain();
            return entry.descriptor;
        }
    return nullptr;
}

IOReturn NativeMaliXnuCache::maintain(void *cpu, size_t bytes,
    NativeMaliCacheOperation operation)
{
    if(!cpu || bytes == 0)return kIOReturnBadArgument;
    if(operation != NativeMaliCacheClean &&
        operation != NativeMaliCacheInvalidate &&
        operation != NativeMaliCacheCleanInvalidate)
        return kIOReturnBadArgument;

    const uintptr_t address = reinterpret_cast<uintptr_t>(cpu);
    if(bytes > UINTPTR_MAX - address)return kIOReturnBadArgument;
    /* The clean paths round the end up to a page; refuse instead of wrapping. */
    if(address + bytes > UINTPTR_MAX - (NativeMaliVmGranule - 1))
        return kIOReturnBadArgument;
    const uintptr_t begin = address & ~static_cast<uintptr_t>(NativeMaliVmGranule - 1);
    const uintptr_t end = (address + bytes + NativeMaliVmGranule - 1) &
        ~static_cast<uintptr_t>(NativeMaliVmGranule - 1);

    /* Invalidate discards the CPU's copy, including a neighbour's dirty line in
     * the same page, so it is only allowed on whole pages. Clean and
     * clean+invalidate may round outward: writing a neighbour's line back is
     * harmless. NativeAdrenoCpu.cpp states the same rule for owned pages.
     * UNVERIFIED(HW): the sequence itself has not run on this phone, and the
     * CPU/GPU coherency policy that drives it is still open
     * (docs/UNVERIFIED.md, docs/OPEN_QUESTIONS.md item 5). */
    if(operation == NativeMaliCacheInvalidate &&
        (begin != address || end != address + bytes))
        return kIOReturnBadArgument;

    disable_preemption();
    uint64_t ctr = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const size_t line = static_cast<size_t>(4) << ((ctr >> 16) & 15);
    if(line == 0 || line > NativeMaliVmGranule) {
        enable_preemption();
        return kIOReturnUnsupported;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    for(uintptr_t at = begin; at < end; at += line) {
        if(operation == NativeMaliCacheClean)
            __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
        else if(operation == NativeMaliCacheInvalidate)
            __asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
        else
            __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
    enable_preemption();
    return kIOReturnSuccess;
}
