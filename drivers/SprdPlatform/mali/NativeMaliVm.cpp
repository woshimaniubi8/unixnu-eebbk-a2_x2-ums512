/* SPDX-License-Identifier: GPL-2.0-only */
/* Descriptor encoding and table walking for ARM_MALI_LPAE; see NativeMaliVm.h
 * for every fact and where it comes from. Nothing here touches hardware. */
#include "NativeMaliVm.h"
#include <string.h>

namespace {

constexpr uint64_t kTypeMask = 0x3;
constexpr uint64_t kTypeBlock = 1;   /* leaf, at every level for ARM_MALI_LPAE */
constexpr uint64_t kTypeTable = 3;
constexpr uint64_t kAddressMask = 0x0000fffffffff000ull;   /* [47:12] */
constexpr uint32_t kMaxLevels = 4;
constexpr uint32_t kGranuleShift = 12;

constexpr uint64_t kHapRead = 1ull << 6;
constexpr uint64_t kHapWrite = 2ull << 6;
constexpr uint64_t kAttrIndexShift = 2;
constexpr uint64_t kAttrIndexCache = 1;
constexpr uint64_t kAttrIndexDevice = 2;
constexpr uint64_t kShareOuter = 2ull << 8;
constexpr uint64_t kExecuteNever = 3ull << 53;

/* Address mode 3 (table) comes with encodeTable()'s type field, so the only
 * extra TRANSTAB bits are these two. */
constexpr uint64_t kTtbrReadInner = 1ull << 2;
constexpr uint64_t kTtbrShareOuter = 1ull << 4;
constexpr uint64_t kMemattrImplementationDefined = 0x88ull;
constexpr uint64_t kMemattrWriteAllocate = 0x8dull;

} // namespace

uint64_t NativeMaliVm::encodePhysical(uint64_t paddr)
{
    /* paddr_to_iopte(): the overlapping RES0 bits are folded out, so a 40-bit
     * address (the Mali maximum) comes out unchanged. */
    return (paddr | (paddr >> (48 - 12))) & kAddressMask;
}

uint64_t NativeMaliVm::decodePhysical(uint64_t pte)
{
    /* Granule is 4 KiB, so no packed high-order bits to rotate back. */
    return pte & kAddressMask;
}

uint64_t NativeMaliVm::encodeLeaf(uint64_t paddr, uint32_t prot)
{
    uint64_t pte = kTypeBlock;
    if(prot & NativeMaliVmRead)pte |= kHapRead;
    if(prot & NativeMaliVmWrite)pte |= kHapWrite;
    if(prot & NativeMaliVmMmio)pte |= kAttrIndexDevice << kAttrIndexShift;
    else if(prot & NativeMaliVmCached)pte |= kAttrIndexCache << kAttrIndexShift;
    /* Always outer-shareable: the source only chooses inner-shareable for the
     * generic formats, and this is ARM_MALI_LPAE. */
    pte |= kShareOuter;
    if(prot & NativeMaliVmNoExec)pte |= kExecuteNever;
    /* No AF and no nG bit: both are skipped for ARM_MALI_LPAE. */
    return pte | encodePhysical(paddr);
}

uint64_t NativeMaliVm::encodeTable(uint64_t table_phys)
{
    return encodePhysical(table_phys) | kTypeTable;
}

NativeMaliVmGeometry NativeMaliVm::geometry(uint32_t ias, uint32_t oas)
{
    NativeMaliVmGeometry g{};
    g.ias = ias;
    g.oas = oas;
    g.bits_per_level = NativeMaliVmBitsPerLevel;
    const uint32_t va_bits = ias > kGranuleShift ? ias - kGranuleShift : 0;
    uint32_t levels = (va_bits + g.bits_per_level - 1) / g.bits_per_level;
    if(levels < 1)levels = 1;
    if(levels > kMaxLevels)levels = kMaxLevels;
    g.start_level = kMaxLevels - levels;
    g.pgd_bits = va_bits - g.bits_per_level * (levels - 1);
    /* "Mali seems to need a full 4-level table regardless of IAS": a walk that
     * would start below level 0 starts at level 0 with one valid entry. */
    if(g.start_level > 0) {
        g.start_level = 0;
        g.pgd_bits = 0;
    }
    g.levels = kMaxLevels - g.start_level;
    g.span = 1ull << (kGranuleShift + g.bits_per_level * (g.levels - 1) + g.pgd_bits);
    return g;
}

uint32_t NativeMaliVm::shift(uint32_t level) const
{
    return (kMaxLevels - level) * geometry_.bits_per_level + 3;
}

uint32_t NativeMaliVm::indexBits(uint32_t level) const
{
    return level == geometry_.start_level ? geometry_.pgd_bits : geometry_.bits_per_level;
}

uint32_t NativeMaliVm::index(uint64_t iova, uint32_t level) const
{
    const uint32_t bits = indexBits(level);
    if(!bits)return 0;
    return uint32_t((iova >> shift(level)) & ((UINT64_C(1) << bits) - 1));
}

IOReturn NativeMaliVm::allocate(uint64_t **table, uint64_t *phys)
{
    if(tables_ >= NativeMaliVmMaxTables || !source_)return kIOReturnNoResources;
    NativeMaliPage page{};
    const IOReturn result = source_->alloc(&page);
    if(result != kIOReturnSuccess)return result;
    if(!page.cpu || (page.phys & (NativeMaliVmGranule - 1)) ||
        page.bytes < NativeMaliVmGranule) {
        /* A source that cannot honour the contract is not quietly trusted. */
        source_->release(page);
        return kIOReturnBadArgument;
    }
    memset(page.cpu, 0, NativeMaliVmGranule);
    page_[tables_] = page;
    *table = static_cast<uint64_t *>(page.cpu);
    *phys = page.phys;
    ++tables_;
    return kIOReturnSuccess;
}

IOReturn NativeMaliVm::descend(uint64_t iova, uint64_t **leaf)
{
    uint64_t *table = pgd_;
    for(uint32_t level = 0; level < kMaxLevels; ++level) {
        uint64_t *slot = &table[index(iova, level)];
        if(level == kMaxLevels - 1) {
            *leaf = slot;
            return kIOReturnSuccess;
        }
        const uint64_t entry = *slot;
        if(!entry) {
            uint64_t *next = nullptr;
            uint64_t phys = 0;
            const IOReturn result = allocate(&next, &phys);
            if(result != kIOReturnSuccess)return result;
            *slot = encodeTable(phys);
            table = next;
        } else if((entry & kTypeMask) == kTypeTable) {
            uint64_t *next = nullptr;
            for(size_t slot_index = 0; slot_index < tables_; ++slot_index)
                if(page_[slot_index].phys == decodePhysical(entry)) {
                    next = static_cast<uint64_t *>(page_[slot_index].cpu);
                    break;
                }
            if(!next)return kIOReturnIOError;   /* a table this space never made */
            table = next;
        } else {
            /* A leaf blocks the way down: mapping over it would leak it. */
            return kIOReturnBusy;
        }
    }
    return kIOReturnIOError;
}

/* The read-only twin of descend(): it never allocates. */
static IOReturn find(const NativeMaliPage *pages, size_t count, uint64_t *pgd,
    const NativeMaliVmGeometry &geometry, uint64_t iova, uint64_t **leaf)
{
    auto shift_of = [&](uint32_t level) { return (4 - level) * geometry.bits_per_level + 3; };
    auto bits_of = [&](uint32_t level) {
        return level == geometry.start_level ? geometry.pgd_bits : geometry.bits_per_level;
    };
    uint64_t *table = pgd;
    for(uint32_t level = 0; level < 4; ++level) {
        const uint32_t bits = bits_of(level);
        const uint32_t slot = bits ? uint32_t((iova >> shift_of(level)) &
            ((UINT64_C(1) << bits) - 1)) : 0;
        if(level == 3) {
            *leaf = &table[slot];
            return kIOReturnSuccess;
        }
        const uint64_t entry = table[slot];
        if(!entry) {
            *leaf = nullptr;
            return kIOReturnSuccess;
        }
        if((entry & 3) != 3) {
            *leaf = nullptr;   /* a leaf above the last level: not ours */
            return kIOReturnSuccess;
        }
        uint64_t *next = nullptr;
        for(size_t i = 0; i < count; ++i)
            if(pages[i].phys == (entry & UINT64_C(0x0000fffffffff000))) {
                next = static_cast<uint64_t *>(pages[i].cpu);
                break;
            }
        if(!next)return kIOReturnIOError;
        table = next;
    }
    return kIOReturnIOError;
}

NativeMaliVm::~NativeMaliVm()
{
    reset();
}

IOReturn NativeMaliVm::prepare(NativeMaliPageSource *source, uint32_t ias, uint32_t oas,
    bool coherent_walk, NativeMaliCacheMaintainer *maintainer)
{
    if(prepared_)return kIOReturnBusy;
    if(!source)return kIOReturnBadArgument;
    if(ias < NativeMaliVmMinIas || ias > NativeMaliVmMaxIas)return kIOReturnUnsupported;
    if(oas < NativeMaliVmMinIas || oas > NativeMaliVmMaxOas)return kIOReturnUnsupported;
    source_ = source;
    maintainer_ = maintainer;
    coherent_ = coherent_walk;
    geometry_ = geometry(ias, oas);
    uint64_t *root = nullptr;
    uint64_t root_phys = 0;
    IOReturn result = allocate(&root, &root_phys);
    if(result != kIOReturnSuccess) {
        source_ = nullptr;   /* nothing was allocated, so nothing to release */
        pgd_ = nullptr;
        return result;
    }
    pgd_ = root;
    prepared_ = true;
    /* io-pgtable-arm.c:1263: the empty PGD is made visible before TRANSTAB may
     * be written. With a maintainer that is a clean, otherwise the caller's
     * job. A failure here tears everything back down. */
    result = publishTables();
    if(result != kIOReturnSuccess) {
        reset();
        return result;
    }
    return kIOReturnSuccess;
}

IOReturn NativeMaliVm::publishTables()
{
    if(!prepared_ || coherent_ || !maintainer_)return kIOReturnSuccess;
    for(size_t i = 0; i < tables_; ++i) {
        const IOReturn result = maintainer_->maintain(page_[i].cpu,
            NativeMaliVmGranule, NativeMaliCacheClean);
        if(result != kIOReturnSuccess)return result;
    }
    return kIOReturnSuccess;
}

void NativeMaliVm::reset()
{
    if(source_)for(size_t i = 0; i < tables_; ++i)source_->release(page_[i]);
    memset(page_, 0, sizeof(page_));
    tables_ = 0;
    pgd_ = nullptr;
    source_ = nullptr;
    maintainer_ = nullptr;
    prepared_ = false;
    coherent_ = false;
    mapped_pages_ = 0;
    geometry_ = NativeMaliVmGeometry{};
}

uint64_t NativeMaliVm::transtab() const
{
    if(!prepared_)return 0;
    uint64_t value = encodeTable(page_[0].phys) | kTtbrReadInner;
    if(coherent_)value |= kTtbrShareOuter;
    return value;
}

uint64_t NativeMaliVm::memattr() const
{
    return (kMemattrImplementationDefined << 0) |
        (kMemattrWriteAllocate << 8) |
        (kMemattrImplementationDefined << 16);
}

/* Alignment, non-zero size, no wrap, inside the address space and inside the
 * table layout. The physical end is checked by map(), which is the only caller
 * that has one. */
static IOReturn checkRange(const NativeMaliVmGeometry &geometry, uint64_t iova,
    uint64_t bytes)
{
    if(!bytes || (iova & (NativeMaliVmGranule - 1)) ||
        (bytes & (NativeMaliVmGranule - 1)))
        return kIOReturnBadArgument;
    if(iova + bytes < iova)return kIOReturnBadArgument;
    if(iova + bytes > (UINT64_C(1) << geometry.ias))return kIOReturnBadArgument;
    if(iova + bytes > geometry.span)return kIOReturnBadArgument;
    return kIOReturnSuccess;
}

IOReturn NativeMaliVm::map(uint64_t iova, uint64_t paddr, uint64_t bytes, uint32_t prot)
{
    if(!prepared_)return kIOReturnNotReady;
    if(!(prot & (NativeMaliVmRead | NativeMaliVmWrite)))return kIOReturnBadArgument;
    if(prot & NativeMaliVmMmio && prot & NativeMaliVmCached)return kIOReturnBadArgument;
    if(paddr & (NativeMaliVmGranule - 1))return kIOReturnBadArgument;
    if(paddr + bytes < paddr)return kIOReturnBadArgument;
    if(paddr + bytes > (UINT64_C(1) << geometry_.oas))return kIOReturnBadArgument;
    IOReturn result = checkRange(geometry_, iova, bytes);
    if(result != kIOReturnSuccess)return result;
    /* Two passes: refuse to overwrite anything, then write. A refusal therefore
     * leaves no half-applied range behind. Tables created by the check stay
     * allocated; only reset() takes them back. */
    for(uint64_t offset = 0; offset < bytes; offset += NativeMaliVmGranule) {
        uint64_t *leaf = nullptr;
        result = descend(iova + offset, &leaf);
        if(result != kIOReturnSuccess)return result;
        if(*leaf)return kIOReturnBusy;
    }
    for(uint64_t offset = 0; offset < bytes; offset += NativeMaliVmGranule) {
        uint64_t *leaf = nullptr;
        result = descend(iova + offset, &leaf);
        if(result != kIOReturnSuccess)return result;
        *leaf = encodeLeaf(paddr + offset, prot);
        ++mapped_pages_;
    }
    /* Publish the leaves (and any table installed on the way down) before the
     * caller can program or flush anything. A failed publish takes the range
     * back out and returns the maintainer's error unchanged. */
    result = publishTables();
    if(result != kIOReturnSuccess) {
        (void)unmap(iova, bytes);
        return result;
    }
    return kIOReturnSuccess;
}

IOReturn NativeMaliVm::unmap(uint64_t iova, uint64_t bytes)
{
    if(!prepared_)return kIOReturnNotReady;
    const IOReturn result = checkRange(geometry_, iova, bytes);
    if(result != kIOReturnSuccess)return result;
    for(uint64_t offset = 0; offset < bytes; offset += NativeMaliVmGranule) {
        uint64_t *leaf = nullptr;
        const IOReturn walked = find(page_, tables_, pgd_, geometry_, iova + offset, &leaf);
        if(walked != kIOReturnSuccess)return walked;
        if(leaf && *leaf) {
            *leaf = 0;
            if(mapped_pages_)--mapped_pages_;
        }
    }
    /* The cleared entries must be visible to the walker before the caller
     * flushes the range; on failure the leaves are already clear, so repeating
     * unmap() only publishes again. */
    return publishTables();
}

IOReturn NativeMaliVm::descriptor(uint64_t iova, uint64_t *pte) const
{
    if(!pte)return kIOReturnBadArgument;
    *pte = 0;
    if(!prepared_)return kIOReturnNotReady;
    if(iova >= geometry_.span)return kIOReturnBadArgument;
    uint64_t *leaf = nullptr;
    const IOReturn walked = find(page_, tables_, pgd_, geometry_, iova, &leaf);
    if(walked != kIOReturnSuccess)return walked;
    if(leaf)*pte = *leaf;
    return kIOReturnSuccess;
}
