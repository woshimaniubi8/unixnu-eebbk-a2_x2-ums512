/* SPDX-License-Identifier: GPL-2.0-only */
/* Buffer objects: CPU memory with a physical span list, mapped into one GPU
 * address space, plus the cache maintenance a non-coherent GPU needs.
 *
 * Source (content-pinned, third_party/PINNED.md):
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:466-493
 *     panfrost_mmu_map(): prot = IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE (plus
 *     IOMMU_NOEXEC for a noexec BO); every DMA segment of the sg table is
 *     mapped at the next IOVA and the whole range is flushed once afterwards.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:421-464
 *     mmu_map_sg() maps one sg segment at a time from the segment's DMA
 *     address, advancing IOVA by the mapped length; on an error it unmaps the
 *     range it already mapped and returns the original error.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:500-524
 *     panfrost_mmu_unmap(): clear the leaves, then flush the range once.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:389-404
 *     the range flush is LOCK + FLUSH_PT, i.e. NativeMaliMmu::flushRange().
 *   drivers/gpu/drm/panfrost/panfrost_device.c:269
 *     pfdev->coherent is true only when the device's DMA attribute says
 *     DEV_DMA_COHERENT. The MT6768 device tree has no "dma-coherent" property
 *     (tests/data/mt6768.dtb; docs/DEVICE.md), so this phone is non-coherent:
 *     the CPU must clean before the GPU reads and invalidate after it writes,
 *     which is also what the M5 roadmap text prescribes.
 *   drivers/iommu/io-pgtable-arm.c
 *     __arm_lpae_sync_pte() / __arm_lpae_clear_pte() publish PTE writes with
 *     dma_sync_single_for_device() when the walker is not coherent; prepare()
 *     takes an optional NativeMaliCacheMaintainer (NativeMaliCache.h) and then
 *     cleans every table page after prepare(), map() and unmap(). Passing the
 *     same maintainer the buffer uses keeps the two halves of the DMA map in
 *     one place. Without one, that publish stays the caller's job
 *     (docs/OPEN_QUESTIONS.md item 5 covers the missing XNU primitive).
 *
 * Deliberately not here: allocation, pinning and CPU mapping behind the
 * NativeMaliMemorySource interface (IOBufferMemoryDescriptor on XNU, a fake in
 * the host tests), and any policy about when a buffer is read or written by a
 * job. That belongs to the submission path (M6).
 *
 * Only 4 KiB leaves are mapped, because that is all NativeMaliVm implements;
 * the source's get_pgsize() would pick 2 MiB and 1 GiB blocks where it can, so
 * a large buffer produces more descriptors here than panfrost would.
 */
#ifndef NATIVE_MALI_MEMORY_H
#define NATIVE_MALI_MEMORY_H
#include "NativeMaliCache.h"
#include "NativeMaliMmu.h"
#include "NativeMaliVm.h"
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* One physically contiguous run of the buffer. `offset` is inside the buffer,
 * `physical` is what the GPU address space maps, both 4 KiB aligned. The spans
 * must be contiguous in offset order and cover the whole buffer exactly. */
struct NativeMaliMemorySpan {
    uint64_t offset;
    uint64_t physical;
    uint64_t bytes;
};

/* How many spans one buffer may have. A caller that walks off the limit gets
 * kIOReturnNoResources, never a silent truncation. 64 spans of the smallest
 * useful mapping cover a 1 MiB buffer at 16 KiB pages. */
enum : size_t { NativeMaliMemoryMaxSpans = 1024 };

/* Buffer storage. alloc() must return page-aligned memory; the implementation
 * keeps the CPU pointer, the span list and the count valid until release().
 * The buffer object never writes into the memory before the caller does. */
class NativeMaliMemorySource {
public:
    NativeMaliMemorySource() = default;
    virtual ~NativeMaliMemorySource() = default;
    NativeMaliMemorySource(const NativeMaliMemorySource &) = delete;
    NativeMaliMemorySource &operator=(const NativeMaliMemorySource &) = delete;

    /* `spans` holds NativeMaliMemoryMaxSpans entries. */
    virtual IOReturn alloc(size_t bytes, void **cpu, NativeMaliMemorySpan *spans,
        size_t *count) = 0;
    virtual void release(void *cpu, const NativeMaliMemorySpan *spans,
        size_t count) = 0;
};

class NativeMaliMemory final {
    NativeMaliMemorySource *source_ = nullptr;
    NativeMaliCacheMaintainer *maintainer_ = nullptr;
    NativeMaliMemorySpan span_[NativeMaliMemoryMaxSpans]{};
    size_t spans_ = 0;
    void *cpu_ = nullptr;
    size_t bytes_ = 0;
    bool coherent_ = false;
    bool prepared_ = false;

    NativeMaliVm *vm_ = nullptr;
    NativeMaliMmu *mmu_ = nullptr;
    uint32_t as_ = 0;
    uint64_t iova_ = 0;
    bool mapped_ = false;
public:
    NativeMaliMemory() = default;
    ~NativeMaliMemory();
    NativeMaliMemory(const NativeMaliMemory &) = delete;
    NativeMaliMemory &operator=(const NativeMaliMemory &) = delete;

    /* Allocate `bytes` from `source` and validate its spans: page-aligned,
     * page-multiple, contiguous from 0, covering exactly `bytes`, with no
     * address wrapping. The output-address-space limit is checked by map()
     * through NativeMaliVm. A buffer that is not coherent needs a maintainer;
     * without one there is no way to honour the clean/invalidate policy, so
     * prepare() refuses rather than skipping it. A second prepare() without
     * reset() returns kIOReturnBusy. */
    IOReturn prepare(NativeMaliMemorySource *source, size_t bytes,
        bool cpuCoherent, NativeMaliCacheMaintainer *maintainer);
    /* Give the allocation back, whether or not it is mapped, and forget every
     * mapping without touching a register. The owner calls this after the
     * address space is gone; release() is the checked form. */
    void reset();
    /* release() refuses while mapped, because the leaves would outlive the
     * pages they point at. */
    IOReturn release();
    bool prepared() const { return prepared_; }
    bool mapped() const { return mapped_; }
    bool coherent() const { return coherent_; }
    void *cpu() const { return cpu_; }
    size_t bytes() const { return bytes_; }
    size_t spans() const { return spans_; }
    uint64_t iova() const { return mapped_ ? iova_ : 0; }

    /* Map the spans at `iova` and flush that range once, the way
     * panfrost_mmu_map() does. `prot` defaults to read/write/cached, the
     * source's IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE. The address space must
     * be present and allocated, so the flush has a live AS to talk to; every
     * mapping error rolls back the part already mapped and returns the first
     * error unchanged. Mapping a buffer twice is refused.
     *
     * `vm` should have been prepared with the same cache maintainer; when it
     * was not, the caller owns publishing the page tables (NativeMaliVm.h). */
    IOReturn map(NativeMaliVm *vm, NativeMaliMmu *mmu, uint32_t as,
        uint64_t iova, uint32_t prot = NativeMaliVmRead | NativeMaliVmWrite |
            NativeMaliVmCached);
    /* Clear the leaves and flush the same range once. If the flush fails the
     * buffer stays marked mapped, because the GPU may still hold the old
     * translation and the tables are already clear: calling unmap() again
     * repeats only the flush, which is the retry the source allows. */
    IOReturn unmap();

    /* Clean before the GPU reads [offset, offset + bytes); invalidate after it
     * writes. Both are no-ops on a coherent device, where panfrost's
     * dma_map_sgtable() does nothing either. The range is checked against the
     * buffer; the maintainer sees the unchanged length. */
    IOReturn cleanForDevice(size_t offset, size_t bytes);
    IOReturn invalidateForCpu(size_t offset, size_t bytes);
};
#endif
