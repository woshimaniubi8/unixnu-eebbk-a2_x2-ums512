/* SPDX-License-Identifier: GPL-2.0-only */
/* ARM_MALI_LPAE page tables, built the way panfrost asks Linux to build them.
 *
 * Source (content-pinned, see third_party/PINNED.md):
 *   third_party/linux-panfrost/drivers/iommu/io-pgtable-arm.c
 *     arm_mali_lpae_alloc_pgtable()  format choice, geometry quirk, TRANSTAB/MEMATTR
 *     arm_lpae_alloc_pgtable()       level math (bits_per_level = 9 at 4 KiB)
 *     arm_lpae_prot_to_pte()         the fmt == ARM_MALI_LPAE branch
 *     __arm_lpae_init_pte()          leaf descriptor type
 *     arm_lpae_install_table()       table descriptor type
 *
 * The facts this file encodes, all read from that source, not from memory:
 *
 *   - 4 KiB granule, 8-byte descriptors, 512 descriptors per table, so
 *     bits_per_level = 9 and the index shifts are 39, 30, 21, 12.
 *   - Mali imposes ias <= 48 and oas <= 40, and refuses every quirk.
 *   - "Mali seems to need a full 4-level table regardless of IAS": when the
 *     generic math would start at level > 0, start_level becomes 0 and pgd_bits
 *     becomes 0, leaving a single valid level-0 entry.
 *   - Leaf descriptors are type 1 (BLOCK) at *every* level, including the last;
 *     the generic ARM format would use type 3 (PAGE) at the last level there.
 *     Table descriptors are type 3.
 *   - Permissions are placed like a stage-2 entry: read/write in [7:6]
 *     (00 fault, 01 read-only, 10 write-only, 11 read-write).
 *   - Attribute index in [4:2]: 1 (write-allocate) for cached memory, 2
 *     (device) for MMIO, 0 otherwise.
 *   - Shareability is always outer-shareable, 2 << 8: the source only picks
 *     inner-shareable for non-Mali formats.
 *   - No XN bit (3 << 53) unless asked for no-execute; no AF bit (1 << 10) and
 *     no nG bit (1 << 11) at all — both are skipped for ARM_MALI_LPAE.
 *   - MEMATTR = 0x88 | 0x8D << 8 | 0x88 << 16; TRANSTAB = pgd | 3 | 4, plus 16
 *     when the walker is coherent.
 *
 * Which format, from panfrost_mmu.c panfrost_mmu_ctx_create(): the AArch64 4K
 * format is chosen only when the board data sets GPU_QUIRK_FORCE_AARCH64_PGTABLE
 * (mt8188, mt8192 and mt8370 in panfrost_drv.c) *and* the core reports
 * HW_FEATURE_AARCH64_MMU; every other board gets ARM_MALI_LPAE, which is what
 * this file implements. MT6768 is not in that list, so the Mali format is the
 * one this GPU needs. (That is also why the AArch64 path's mair_to_memattr()
 * and its TRANSCFG bits are not modelled here.)
 *
 * Left for NativeMaliMmu, which cannot run before the MFG domains are up (M2):
 * AS_TRANSCFG = AS_TRANSCFG_ADRMODE_LEGACY (0), then AS_COMMAND_UPDATE (1) to
 * broadcast TRANSTAB and MEMATTR — panfrost_mmu.c mmu_cfg_init_mali_lpae() and
 * panfrost_regs.h AS_COMMAND_*.
 *
 * This object only builds descriptors in memory the caller owns. It programs no
 * MMU register, flushes no cache and powers nothing: writing TRANSTAB or
 * AS_COMMAND belongs to NativeMaliMmu and needs the MFG power domains up (M2).
 * Table pages must be zeroed by the CPU and, before the MMU walks them, written
 * back to the point of coherency (io-pgtable-arm.c __arm_lpae_sync_pte()). When
 * the walker is not coherent, prepare() can be given a
 * NativeMaliCacheMaintainer and then every table page is cleaned after
 * prepare(), map() and unmap(); the source's dma_sync_single_for_device() has no
 * pinned XNU counterpart yet (docs/OPEN_QUESTIONS.md item 5), so without a
 * maintainer that publish stays the caller's job.
 *
 * Not implemented on purpose: 2 MiB and 1 GiB block mappings, freeing a table
 * once its last leaf is unmapped (reset() releases everything), and the
 * IO_PGTABLE_QUIRK_ARM_HD dirty-bit path. None of them is needed to submit a
 * WRITE_VALUE job, and each would be another untested branch on hardware we
 * cannot yet touch.
 */
#ifndef NATIVE_MALI_VM_H
#define NATIVE_MALI_VM_H
#include "NativeMaliCache.h"
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

enum : uint64_t {
    NativeMaliVmGranule = 4096,
    NativeMaliVmPteBytes = 8,
    NativeMaliVmPtesPerTable = 512
};
enum : uint32_t {
    NativeMaliVmMaxIas = 48,
    NativeMaliVmMaxOas = 40,
    NativeMaliVmMinIas = 32,
    NativeMaliVmBitsPerLevel = 9
};
/* The 1 + up-to-4 tables of one address space, plus slack for the tests'
 * scattered mappings. A caller that walks off this limit gets an error, never a
 * silent truncation. */
enum : size_t { NativeMaliVmMaxTables = 64 };

/* Permissions as the encoder consumes them. Read and write are the [7:6] pair;
 * Cached selects attribute index 1, Mmio index 2; NoExec sets XN. */
enum NativeMaliVmProt : uint32_t {
    NativeMaliVmRead = 1u << 0,
    NativeMaliVmWrite = 1u << 1,
    NativeMaliVmCached = 1u << 2,
    NativeMaliVmNoExec = 1u << 3,
    NativeMaliVmMmio = 1u << 4
};

/* One page-table page: the CPU address used to fill it in, and the physical
 * address the GPU's table walk reads. Both are the caller's; this object never
 * maps, allocates or guesses them. */
struct NativeMaliPage {
    void *cpu;
    uint64_t phys;
    size_t bytes;
};

/* Table storage. Implementations must hand out 4 KiB, physically contiguous,
 * page-aligned memory and must not touch its contents; the builder zeroes it. */
class NativeMaliPageSource {
public:
    NativeMaliPageSource() = default;
    virtual ~NativeMaliPageSource() = default;
    NativeMaliPageSource(const NativeMaliPageSource &) = delete;
    NativeMaliPageSource &operator=(const NativeMaliPageSource &) = delete;
    virtual IOReturn alloc(NativeMaliPage *page) = 0;
    virtual void release(const NativeMaliPage &page) = 0;
};

struct NativeMaliVmGeometry {
    uint32_t ias;
    uint32_t oas;
    uint32_t start_level;
    uint32_t levels;
    uint32_t bits_per_level;
    uint32_t pgd_bits;
    uint64_t span;
};

class NativeMaliVm final {
    struct Table {
        uint64_t *pte;
        uint64_t phys;
        uint32_t slot;
    };
    NativeMaliPageSource *source_ = nullptr;
    NativeMaliCacheMaintainer *maintainer_ = nullptr;
    NativeMaliPage page_[NativeMaliVmMaxTables]{};
    size_t tables_ = 0;
    NativeMaliVmGeometry geometry_{};
    bool coherent_ = false;
    bool prepared_ = false;
    uint64_t *pgd_ = nullptr;
    uint64_t mapped_pages_ = 0;

    uint32_t shift(uint32_t level) const;
    uint32_t indexBits(uint32_t level) const;
    uint32_t index(uint64_t iova, uint32_t level) const;
    IOReturn allocate(uint64_t **table, uint64_t *phys);
    IOReturn descend(uint64_t iova, uint64_t **leaf);
    /* Publish every table page with NativeMaliCacheClean when the walker is not
     * coherent; a no-op when it is coherent or when no maintainer was given. */
    IOReturn publishTables();
public:
    NativeMaliVm() = default;
    ~NativeMaliVm();
    NativeMaliVm(const NativeMaliVm &) = delete;
    NativeMaliVm &operator=(const NativeMaliVm &) = delete;

    /* Level math for one (ias, oas) pair, with the Mali 4-level quirk applied.
     * Refuses nothing; prepare() does the validating. */
    static NativeMaliVmGeometry geometry(uint32_t ias, uint32_t oas);

    /* Take ownership of the root table (allocated from `source`). One address
     * space per object: prepare() refuses a second call until reset().
     * `maintainer` is optional: for a non-coherent walker it publishes the root
     * table (the empty PGD the source makes visible before TRANSTAB with a
     * wmb(), io-pgtable-arm.c:1263) and, later, every table page map()/unmap()
     * touch. Without one, the caller owns that publish step; that is the state
     * all M4 tests use. A coherent walker never needs either. */
    IOReturn prepare(NativeMaliPageSource *source, uint32_t ias, uint32_t oas,
        bool coherent_walk, NativeMaliCacheMaintainer *maintainer = nullptr);
    /* Release every table back to the source and forget every mapping. */
    void reset();

    bool prepared() const { return prepared_; }
    const NativeMaliVmGeometry &info() const { return geometry_; }
    /* Value for AS_TRANSTAB: root table address plus address mode 3 (table),
     * read-inner, and outer-shareable when the walker is coherent. */
    uint64_t transtab() const;
    /* Value for AS_MEMATTR: implementation-defined, write-allocate, and
     * implementation-defined for the device index, packed 8 bits each. */
    uint64_t memattr() const;
    size_t tables() const { return tables_; }
    uint64_t mappedPages() const { return mapped_pages_; }

    /* Map `bytes` of `paddr` at `iova`, both 4 KiB aligned, iova inside the
     * space. Overwriting a live leaf or a table is refused, exactly as
     * arm_lpae_init_pte() refuses it (-EEXIST), because it would leak the
     * mapping underneath. */
    IOReturn map(uint64_t iova, uint64_t paddr, uint64_t bytes, uint32_t prot);
    /* Clear the leaves of a range that was mapped. Empty tables stay allocated
     * until reset(); that is the documented difference from Linux. */
    IOReturn unmap(uint64_t iova, uint64_t bytes);
    /* The leaf descriptor covering `iova` (0 when nothing is mapped there). */
    IOReturn descriptor(uint64_t iova, uint64_t *pte) const;

    /* Pure encoders, so tests can check the bit layout without a page source. */
    static uint64_t encodeLeaf(uint64_t paddr, uint32_t prot);
    static uint64_t encodeTable(uint64_t table_phys);
    /* paddr_to_iopte(): pack the address into [47:12]. A 40-bit physical
     * address, the Mali maximum, is carried unchanged. */
    static uint64_t encodePhysical(uint64_t paddr);
    /* Physical address a leaf descriptor points at. */
    static uint64_t decodePhysical(uint64_t pte);
};
#endif
