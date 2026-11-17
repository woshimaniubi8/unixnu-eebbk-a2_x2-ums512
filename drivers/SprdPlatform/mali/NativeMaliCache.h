/* SPDX-License-Identifier: GPL-2.0-only */
/* CPU cache maintenance, shared by the page tables and the data buffers.
 *
 * Source (content-pinned, third_party/PINNED.md):
 *   drivers/iommu/io-pgtable-arm.c:350-364, 380-390, 440-450, 490-500
 *     __arm_lpae_sync_pte() and __arm_lpae_clear_pte(): with
 *     cfg->coherent_walk == false every PTE write and every cleared entry is
 *     published with dma_sync_single_for_device(..., DMA_TO_DEVICE); the
 *     coherent path skips it entirely.
 *   drivers/iommu/io-pgtable-arm.c:255-300, 305-345
 *     a table page is dma_map_single()d (or brought up as incoherent by
 *     iommu_pages_start_incoherent()) at allocation when the walker is not
 *     coherent, and unmapped again on free.
 *   drivers/gpu/drm/panfrost/panfrost_device.c:269 and panfrost_mmu.c:820
 *     pfdev->coherent becomes cfg->coherent_walk; on this phone it is false
 *     (no "dma-coherent" in the device tree, docs/OPEN_QUESTIONS.md item 13).
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:466-493
 *     a BO's pages are DMA-mapped (drm_gem_shmem_get_pages_sgt() ->
 *     dma_map_sgtable()) before the GPU can see them.
 *
 * What clean/invalidate mean here is the CPU side of dma_sync: clean writes a
 * line back to the point of coherency, invalidate drops the CPU's copy. That is
 * what makes a GPU write visible to the CPU afterwards. The XNU primitives for
 * both (dc cvac / dc ivac, and IOBufferMemoryDescriptor's own DMA mapping) are
 * still an open question (docs/OPEN_QUESTIONS.md item 5), so this is only the
 * interface; the host tests use a recording fake.
 */
#ifndef NATIVE_MALI_CACHE_H
#define NATIVE_MALI_CACHE_H
#include <IOKit/IOTypes.h>
#include <stddef.h>

enum NativeMaliCacheOperation {
    NativeMaliCacheClean,          /* clean to PoC: before the GPU reads */
    NativeMaliCacheInvalidate,     /* invalidate to PoC: after the GPU writes */
    NativeMaliCacheCleanInvalidate
};

/* `cpu` is a CPU address; `bytes` is the caller's length, not rounded to cache
 * lines by this layer. */
class NativeMaliCacheMaintainer {
public:
    NativeMaliCacheMaintainer() = default;
    virtual ~NativeMaliCacheMaintainer() = default;
    NativeMaliCacheMaintainer(const NativeMaliCacheMaintainer &) = delete;
    NativeMaliCacheMaintainer &operator=(const NativeMaliCacheMaintainer &) = delete;

    virtual IOReturn maintain(void *cpu, size_t bytes,
        NativeMaliCacheOperation operation) = 0;
};
#endif
