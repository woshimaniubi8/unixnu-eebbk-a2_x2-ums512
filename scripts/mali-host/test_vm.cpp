/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliVm over an in-memory page source. No device access: this qualifies
 * the descriptor encoding and the table walk against the golden values and the
 * refusals the pinned io-pgtable-arm.c defines, not the MMU. */
#include "NativeMaliVm.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

/* A page source that hands out real 4 KiB allocations and records them, so a
 * test can also prove that every page came back. Pages are filled with a
 * non-zero pattern to prove the builder zeroes what it is given. */
class FakePages final : public NativeMaliPageSource {
public:
    enum : size_t { Capacity = NativeMaliVmMaxTables };
    void *cpu[Capacity]{};
    uint64_t phys[Capacity]{};
    size_t live = 0;
    size_t allocs = 0;
    uint64_t firstPhys = UINT64_C(0x40000000);
    IOReturn alloc(NativeMaliPage *page) override
    {
        if(live == Capacity)return kIOReturnNoMemory;
        const size_t slot = live++;
        void *memory = std::aligned_alloc(NativeMaliVmGranule, NativeMaliVmGranule);
        if(!memory)return kIOReturnNoMemory;
        std::memset(memory, 0xaa, NativeMaliVmGranule);
        cpu[slot] = memory;
        phys[slot] = firstPhys + slot * NativeMaliVmGranule;
        ++allocs;
        page->cpu = memory;
        page->phys = phys[slot];
        page->bytes = NativeMaliVmGranule;
        return kIOReturnSuccess;
    }
    void release(const NativeMaliPage &page) override
    {
        for(size_t slot = 0; slot < live; ++slot)
            if(cpu[slot] == page.cpu) {
                std::free(cpu[slot]);
                --live;
                cpu[slot] = cpu[live];
                phys[slot] = phys[live];
                cpu[live] = nullptr;
                phys[live] = 0;
                return;
            }
        std::fprintf(stderr, "released a page this source never handed out\n");
        std::exit(1);
    }
    bool zeroed(size_t slot) const
    {
        const uint8_t *bytes = static_cast<const uint8_t *>(cpu[slot]);
        for(size_t i = 0; i < NativeMaliVmGranule; ++i)
            if(bytes[i])return false;
        return true;
    }
};

/* A source that breaks the contract: its pages are not aligned. prepare() must
 * refuse it and give the page back rather than build on it. */
class MisalignedPages final : public NativeMaliPageSource {
public:
    size_t live = 0;
    IOReturn alloc(NativeMaliPage *page) override
    {
        void *memory = std::aligned_alloc(NativeMaliVmGranule, NativeMaliVmGranule);
        if(!memory)return kIOReturnNoMemory;
        ++live;
        page->cpu = memory;
        page->phys = UINT64_C(0x40000800);   /* not 4 KiB aligned */
        page->bytes = NativeMaliVmGranule;
        return kIOReturnSuccess;
    }
    void release(const NativeMaliPage &page) override
    {
        std::free(page.cpu);
        --live;
    }
};

/* Records every cache operation the table builder asks for. */
class FakeCache final : public NativeMaliCacheMaintainer {
public:
    struct Record {
        void *cpu;
        size_t bytes;
        NativeMaliCacheOperation operation;
    };
    std::vector<Record> calls;
    IOReturn result = kIOReturnSuccess;

    IOReturn maintain(void *cpu, size_t bytes,
        NativeMaliCacheOperation operation) override
    {
        calls.push_back({cpu, bytes, operation});
        return result;
    }
};

static uint64_t random_state = UINT64_C(0x9e3779b97f4a7c15);
static uint32_t nextRandom()
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return uint32_t(random_state >> 32);
}

static const uint32_t rw_cached = NativeMaliVmRead | NativeMaliVmWrite | NativeMaliVmCached;

int main()
{
    /* ---- geometry: the level math of arm_lpae_alloc_pgtable() ---- */
    {
        const NativeMaliVmGeometry wide = NativeMaliVm::geometry(48, 40);
        CHECK(wide.start_level == 0 && wide.levels == 4);
        CHECK(wide.bits_per_level == 9 && wide.pgd_bits == 9);
        CHECK(wide.span == (UINT64_C(1) << 48));
        /* ias 40: 28 bits over four tables leaves a single-bit root index. */
        const NativeMaliVmGeometry narrow = NativeMaliVm::geometry(40, 40);
        CHECK(narrow.start_level == 0 && narrow.pgd_bits == 1);
        CHECK(narrow.span == (UINT64_C(1) << 40));
        /* ias 39 would start at level 1; Mali forces level 0 with one entry. */
        const NativeMaliVmGeometry small = NativeMaliVm::geometry(39, 40);
        CHECK(small.start_level == 0 && small.pgd_bits == 0 && small.levels == 4);
        CHECK(small.span == (UINT64_C(1) << 39));
        const NativeMaliVmGeometry tiny = NativeMaliVm::geometry(32, 32);
        CHECK(tiny.start_level == 0 && tiny.pgd_bits == 0);
        CHECK(tiny.span == (UINT64_C(1) << 39));   /* layout reach, ias still 32 */
        /* Index shifts are 39, 30, 21, 12 at 4 KiB, at every geometry. */
        for(uint32_t level = 0; level < 4; ++level) {
            const uint32_t expected = (4 - level) * 9 + 3;
            CHECK(expected == (level == 0 ? 39u : level == 1 ? 30u : level == 2 ? 21u : 12u));
        }
    }

    /* ---- descriptors: golden words from arm_lpae_prot_to_pte() ---- */
    {
        /* type 1 (BLOCK, at every level for Mali) + read + write + attr index 1
         * (write-allocate) + outer-shareable. No AF bit, no nG bit. */
        CHECK(NativeMaliVm::encodeLeaf(UINT64_C(0x13040000),
            NativeMaliVmRead | NativeMaliVmWrite | NativeMaliVmCached) == UINT64_C(0x130402c5));
        /* Read-only drops the write half of [7:6]. */
        CHECK(NativeMaliVm::encodeLeaf(UINT64_C(0x13040000),
            NativeMaliVmRead | NativeMaliVmCached) == UINT64_C(0x13040245));
        /* Device memory uses attr index 2 instead of the cache index. */
        CHECK(NativeMaliVm::encodeLeaf(UINT64_C(0x13040000),
            NativeMaliVmRead | NativeMaliVmWrite | NativeMaliVmMmio) == UINT64_C(0x130402c9));
        /* XN is 3 << 53, not 1 << 54. */
        CHECK(NativeMaliVm::encodeLeaf(0,
            NativeMaliVmRead | NativeMaliVmWrite | NativeMaliVmNoExec) ==
            UINT64_C(0x00600000000002c1));
        /* A table descriptor is type 3 over the same address bits. */
        CHECK(NativeMaliVm::encodeTable(UINT64_C(0x40001000)) == UINT64_C(0x40001003));
        /* paddr_to_iopte(): a 40-bit address is carried unchanged; bits below
         * the granule are not part of the descriptor. */
        CHECK(NativeMaliVm::encodePhysical(UINT64_C(0x13fd00000)) == UINT64_C(0x13fd00000));
        CHECK(NativeMaliVm::encodePhysical(UINT64_C(0x13fd00fff)) == UINT64_C(0x13fd00000));
        /* Bits above 47 fold into [15:12], exactly as the macro does. */
        CHECK(NativeMaliVm::encodePhysical(UINT64_C(1) << 48) == UINT64_C(0x1000));
        CHECK(NativeMaliVm::decodePhysical(NativeMaliVm::encodePhysical(UINT64_C(0x13fd00000)))
            == UINT64_C(0x13fd00000));
    }

    /* ---- AS_MEMATTR, AS_TRANSTAB, and what prepare() refuses ---- */
    {
        FakePages pages;
        NativeMaliVm vm;
        CHECK(vm.prepare(&pages, 48, 40, false) == kIOReturnSuccess);
        CHECK(vm.memattr() == UINT64_C(0x00888d88));
        /* Address mode 3 (table) plus read-inner, and no share bit when the
         * walker is not coherent. */
        CHECK(vm.transtab() == (pages.phys[0] | 3 | 4));
        /* The root arrived full of 0xaa and must have been zeroed. */
        CHECK(pages.zeroed(0));
        CHECK(vm.tables() == 1);
        /* One space per object until reset(). */
        CHECK(vm.prepare(&pages, 48, 40, false) == kIOReturnBusy);
        vm.reset();
        CHECK(pages.live == 0);
        CHECK(!vm.prepared());

        /* The coherent walker takes the outer-shareable bit. */
        CHECK(vm.prepare(&pages, 48, 40, true) == kIOReturnSuccess);
        CHECK(vm.transtab() == (pages.phys[0] | 3 | 4 | 16));
        vm.reset();
        CHECK(pages.live == 0);

        /* Mali refuses every quirk and every address width it does not have. */
        CHECK(vm.prepare(&pages, 49, 40, false) == kIOReturnUnsupported);
        CHECK(vm.prepare(&pages, 0, 40, false) == kIOReturnUnsupported);
        CHECK(vm.prepare(&pages, 48, 41, false) == kIOReturnUnsupported);
        CHECK(vm.prepare(&pages, 48, 31, false) == kIOReturnUnsupported);
        CHECK(vm.prepare(nullptr, 48, 40, false) == kIOReturnBadArgument);
        CHECK(pages.live == 0);
        /* A source that cannot align pages is not trusted, and its page is
         * given back. */
        MisalignedPages broken;
        CHECK(vm.prepare(&broken, 48, 40, false) == kIOReturnBadArgument);
        CHECK(broken.live == 0);
        CHECK(!vm.prepared());
    }

    /* ---- mapping, the walk, and table sharing ---- */
    {
        FakePages pages;
        NativeMaliVm vm;
        CHECK(vm.prepare(&pages, 48, 40, false) == kIOReturnSuccess);
        /* root (level 0, 512 GiB per entry), level 1 (1 GiB), level 2 (2 MiB)
         * and level 3 (4 KiB): four tables for the first mapping. */
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0x80000000),
            8 * NativeMaliVmGranule, rw_cached) == kIOReturnSuccess);
        CHECK(vm.tables() == 4);
        CHECK(vm.mappedPages() == 8);
        for(uint64_t page_index = 0; page_index < 8; ++page_index) {
            uint64_t pte = 0;
            CHECK(vm.descriptor(UINT64_C(0x10000000) + page_index * NativeMaliVmGranule, &pte)
                == kIOReturnSuccess);
            CHECK(pte == NativeMaliVm::encodeLeaf(UINT64_C(0x80000000) +
                page_index * NativeMaliVmGranule, rw_cached));
            /* The leaf really is a type-1 ("block") descriptor at the last
             * level, which is the Mali difference from the generic format. */
            CHECK((pte & 3) == 1);
            CHECK(NativeMaliVm::decodePhysical(pte) ==
                UINT64_C(0x80000000) + page_index * NativeMaliVmGranule);
        }
        /* A different 2 MiB slot in the same 1 GiB region: only its level-3
         * table is new, because the level-2 table already covers the slot. */
        CHECK(vm.map(UINT64_C(0x20000000), UINT64_C(0x90000000),
            NativeMaliVmGranule, rw_cached) == kIOReturnSuccess);
        CHECK(vm.tables() == 5);
        /* A different 1 GiB region needs its own level-2 and level-3 tables. */
        CHECK(vm.map(UINT64_C(0x80000000), UINT64_C(0xa0000000),
            NativeMaliVmGranule, rw_cached) == kIOReturnSuccess);
        CHECK(vm.tables() == 7);
        CHECK(vm.mappedPages() == 10);

        /* Refusals. Nothing live is overwritten, and a refused call adds no
         * mapping. */
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0xb0000000),
            NativeMaliVmGranule, rw_cached) == kIOReturnBusy);
        CHECK(vm.map(UINT64_C(0x10000000) + 8, UINT64_C(0xb0000000),
            NativeMaliVmGranule, rw_cached) == kIOReturnBadArgument);
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0xb0000000) + 8,
            NativeMaliVmGranule, rw_cached) == kIOReturnBadArgument);
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0xb0000000),
            3 * NativeMaliVmGranule + 4, rw_cached) == kIOReturnBadArgument);
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0xb0000000), 0, rw_cached)
            == kIOReturnBadArgument);
        /* Outside the input address space, and outside the output one. */
        CHECK(vm.map(UINT64_C(1) << 48, UINT64_C(0xb0000000),
            NativeMaliVmGranule, rw_cached) == kIOReturnBadArgument);
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(1) << 40,
            NativeMaliVmGranule, rw_cached) == kIOReturnBadArgument);
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0xb0000000),
            NativeMaliVmGranule, 0) == kIOReturnBadArgument);
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0xb0000000),
            NativeMaliVmGranule, NativeMaliVmCached) == kIOReturnBadArgument);
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0xb0000000),
            NativeMaliVmGranule, NativeMaliVmRead | NativeMaliVmCached | NativeMaliVmMmio)
            == kIOReturnBadArgument);
        /* A wrap in the range is refused before it can walk anywhere. */
        CHECK(vm.map(UINT64_C(0xfffffffffffff000), UINT64_C(0xb0000000),
            NativeMaliVmGranule, rw_cached) == kIOReturnBadArgument);
        CHECK(vm.mappedPages() == 10);
        CHECK(vm.tables() == 7);

        /* Unmapping clears leaves; the empty tables stay until reset(). */
        CHECK(vm.unmap(UINT64_C(0x10000000), 8 * NativeMaliVmGranule) == kIOReturnSuccess);
        CHECK(vm.mappedPages() == 2);
        for(uint64_t page_index = 0; page_index < 8; ++page_index) {
            uint64_t pte = 0xdead;
            CHECK(vm.descriptor(UINT64_C(0x10000000) + page_index * NativeMaliVmGranule, &pte)
                == kIOReturnSuccess);
            CHECK(pte == 0);
        }
        /* The other two mappings are untouched. */
        uint64_t kept = 0;
        CHECK(vm.descriptor(UINT64_C(0x80000000), &kept) == kIOReturnSuccess);
        CHECK(kept == NativeMaliVm::encodeLeaf(UINT64_C(0xa0000000), rw_cached));
        /* Unmapping what was never mapped is not an error. */
        CHECK(vm.unmap(UINT64_C(0x10000000), 8 * NativeMaliVmGranule) == kIOReturnSuccess);
        CHECK(vm.mappedPages() == 2);
        /* The hole can be mapped again. */
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0xc0000000),
            NativeMaliVmGranule, NativeMaliVmRead | NativeMaliVmCached) == kIOReturnSuccess);
        CHECK(vm.mappedPages() == 3);

        vm.reset();
        CHECK(pages.live == 0);
        CHECK(vm.tables() == 0 && vm.mappedPages() == 0);
        /* Nothing is built before prepare(). */
        uint64_t pte = 0xdead;
        CHECK(vm.descriptor(0, &pte) == kIOReturnNotReady);
        CHECK(vm.map(0, 0, NativeMaliVmGranule, rw_cached) == kIOReturnNotReady);
        CHECK(vm.transtab() == 0);
    }

    /* ---- ias 40 uses the single root index bit; ias 39 refuses that address ---- */
    {
        FakePages pages;
        NativeMaliVm vm;
        CHECK(vm.prepare(&pages, 40, 40, false) == kIOReturnSuccess);
        CHECK(vm.map(0, UINT64_C(0x80000000), NativeMaliVmGranule, rw_cached)
            == kIOReturnSuccess);
        CHECK(vm.tables() == 4);
        /* Second root entry: root + two L1 + two L2 + two L3. */
        CHECK(vm.map(UINT64_C(1) << 39, UINT64_C(0x81000000), NativeMaliVmGranule, rw_cached)
            == kIOReturnSuccess);
        CHECK(vm.tables() == 7);
        uint64_t low = 0, high = 0;
        CHECK(vm.descriptor(0, &low) == kIOReturnSuccess);
        CHECK(vm.descriptor(UINT64_C(1) << 39, &high) == kIOReturnSuccess);
        CHECK(low != high);
        CHECK(NativeMaliVm::decodePhysical(high) == UINT64_C(0x81000000));
        vm.reset();
        CHECK(pages.live == 0);

        /* Same address at ias 39 is outside the space. */
        CHECK(vm.prepare(&pages, 39, 40, false) == kIOReturnSuccess);
        CHECK(vm.map(UINT64_C(1) << 39, UINT64_C(0x81000000), NativeMaliVmGranule, rw_cached)
            == kIOReturnBadArgument);
        CHECK(vm.map((UINT64_C(1) << 39) - NativeMaliVmGranule, UINT64_C(0x81000000),
            NativeMaliVmGranule, rw_cached) == kIOReturnSuccess);
        vm.reset();
        CHECK(pages.live == 0);
    }

    /* ---- the table limit is an error, never a truncation ---- */
    {
        FakePages pages;
        NativeMaliVm vm;
        CHECK(vm.prepare(&pages, 48, 40, false) == kIOReturnSuccess);
        /* Every new 2 MiB slot in the one 1 GiB region costs exactly one
         * level-3 table: root, level-1 and level-2 tables plus 61 of them fill
         * the 64-table budget. */
        size_t mapped = 0;
        for(uint64_t slot = 0; slot < 80; ++slot) {
            const IOReturn result = vm.map(UINT64_C(0x10000000) + slot * 0x200000,
                UINT64_C(0x80000000) + slot * 0x200000, NativeMaliVmGranule, rw_cached);
            if(result != kIOReturnSuccess) {
                CHECK(result == kIOReturnNoResources);
                break;
            }
            ++mapped;
        }
        CHECK(mapped == 61);
        CHECK(vm.tables() == NativeMaliVmMaxTables);
        CHECK(vm.mappedPages() == mapped);
        /* Every page that was accepted still walks back correctly. */
        for(uint64_t slot = 0; slot < mapped; ++slot) {
            uint64_t pte = 0;
            CHECK(vm.descriptor(UINT64_C(0x10000000) + slot * 0x200000, &pte)
                == kIOReturnSuccess);
            CHECK(NativeMaliVm::decodePhysical(pte) == UINT64_C(0x80000000) + slot * 0x200000);
        }
        vm.reset();
        CHECK(pages.live == 0);
    }

    /* ---- randomized: map, verify every page, unmap, verify again ---- */
    {
        enum : uint32_t { WindowPages = 2048 };            /* 8 MiB */
        const uint64_t base = UINT64_C(0x40000000);
        FakePages pages;
        NativeMaliVm vm;
        CHECK(vm.prepare(&pages, 48, 40, true) == kIOReturnSuccess);
        static uint64_t expected[WindowPages];
        std::memset(expected, 0, sizeof(expected));
        uint64_t live = 0;
        for(uint32_t attempt = 0; attempt < 400; ++attempt) {
            const uint32_t first = nextRandom() % WindowPages;
            uint32_t count = 1 + nextRandom() % 8;
            if(first + count > WindowPages)count = WindowPages - first;
            const uint64_t iova = base + uint64_t(first) * NativeMaliVmGranule;
            const uint64_t paddr = UINT64_C(0x80000000) +
                uint64_t(nextRandom() % 1024) * NativeMaliVmGranule;
            const IOReturn result = vm.map(iova, paddr, uint64_t(count) * NativeMaliVmGranule,
                rw_cached);
            if(result == kIOReturnBusy)continue;   /* overlapped something live */
            CHECK(result == kIOReturnSuccess);
            for(uint32_t index = 0; index < count; ++index)
                expected[first + index] = paddr + uint64_t(index) * NativeMaliVmGranule;
            live += count;
        }
        CHECK(live > 0);
        CHECK(vm.mappedPages() == live);
        for(uint32_t index = 0; index < WindowPages; ++index) {
            uint64_t pte = 0;
            CHECK(vm.descriptor(base + uint64_t(index) * NativeMaliVmGranule, &pte)
                == kIOReturnSuccess);
            if(expected[index])
                CHECK(NativeMaliVm::decodePhysical(pte) == expected[index]);
            else
                CHECK(pte == 0);
        }
        /* Clear every other mapped page, then check both halves. */
        uint64_t cleared = 0;
        for(uint32_t index = 0; index < WindowPages; ++index)
            if(expected[index] && (index & 1)) {
                CHECK(vm.unmap(base + uint64_t(index) * NativeMaliVmGranule,
                    NativeMaliVmGranule) == kIOReturnSuccess);
                expected[index] = 0;
                ++cleared;
            }
        CHECK(cleared > 0);
        CHECK(vm.mappedPages() == live - cleared);
        for(uint32_t index = 0; index < WindowPages; ++index) {
            uint64_t pte = 0;
            CHECK(vm.descriptor(base + uint64_t(index) * NativeMaliVmGranule, &pte)
                == kIOReturnSuccess);
            CHECK(expected[index] ? NativeMaliVm::decodePhysical(pte) == expected[index] : pte == 0);
        }
        vm.reset();
        CHECK(pages.live == 0);
    }

    /* ---- ranges that cross a 2 MiB and a 1 GiB boundary ---- */
    {
        FakePages pages;
        NativeMaliVm vm;
        CHECK(vm.prepare(&pages, 48, 40, false) == kIOReturnSuccess);
        /* Four pages starting two pages before a 2 MiB boundary: the level-3
         * table has to be created for the second 2 MiB slot in the middle of the
         * range, not at its start. */
        const uint64_t start = UINT64_C(0x02000000) - 2 * NativeMaliVmGranule;
        CHECK(vm.map(start, UINT64_C(0x80000000), 4 * NativeMaliVmGranule, rw_cached)
            == kIOReturnSuccess);
        CHECK(vm.tables() == 5);              /* root, L1, L2, and two L3s */
        CHECK(vm.mappedPages() == 4);
        for(uint64_t index = 0; index < 4; ++index) {
            uint64_t pte = 0;
            CHECK(vm.descriptor(start + index * NativeMaliVmGranule, &pte)
                == kIOReturnSuccess);
            CHECK(NativeMaliVm::decodePhysical(pte) ==
                UINT64_C(0x80000000) + index * NativeMaliVmGranule);
        }
        /* Across a 1 GiB boundary the pages are adjacent but the level-1 index
         * changes: the old region needs a new level-3 table and the next region
         * a new level-2 and level-3 table. Three new tables for two pages. */
        const uint64_t gib = UINT64_C(0x40000000) - NativeMaliVmGranule;
        const size_t before = vm.tables();
        CHECK(vm.map(gib, UINT64_C(0x90000000), 2 * NativeMaliVmGranule, rw_cached)
            == kIOReturnSuccess);
        CHECK(vm.tables() == before + 3);
        CHECK(vm.mappedPages() == 6);
        uint64_t low = 0, high = 0;
        CHECK(vm.descriptor(gib, &low) == kIOReturnSuccess);
        CHECK(vm.descriptor(gib + NativeMaliVmGranule, &high) == kIOReturnSuccess);
        CHECK(NativeMaliVm::decodePhysical(low) == UINT64_C(0x90000000));
        CHECK(NativeMaliVm::decodePhysical(high) == UINT64_C(0x90001000));
        /* Unmapping just the last page leaves its neighbour alone. */
        CHECK(vm.unmap(gib + NativeMaliVmGranule, NativeMaliVmGranule) == kIOReturnSuccess);
        CHECK(vm.descriptor(gib + NativeMaliVmGranule, &high) == kIOReturnSuccess);
        CHECK(high == 0);
        CHECK(vm.descriptor(gib, &low) == kIOReturnSuccess);
        CHECK(NativeMaliVm::decodePhysical(low) == UINT64_C(0x90000000));
        CHECK(vm.mappedPages() == 5);

        /* The last page of a 48-bit space is mappable; one page past it is not. */
        const uint64_t last = (UINT64_C(1) << 48) - NativeMaliVmGranule;
        CHECK(vm.map(last, UINT64_C(0xa0000000), NativeMaliVmGranule, rw_cached)
            == kIOReturnSuccess);
        uint64_t end = 0;
        CHECK(vm.descriptor(last, &end) == kIOReturnSuccess);
        CHECK(NativeMaliVm::decodePhysical(end) == UINT64_C(0xa0000000));
        CHECK(vm.map(last + NativeMaliVmGranule, UINT64_C(0xa0000000),
            NativeMaliVmGranule, rw_cached) == kIOReturnBadArgument);
        CHECK(vm.mappedPages() == 6);
        vm.reset();
        CHECK(pages.live == 0);
    }

    /* ---- page-table publish: the non-coherent walker's dma_sync ---- */
    {
        FakeCache cache;
        FakePages pages;
        NativeMaliVm vm;
        /* prepare() publishes the empty root before TRANSTAB may be written
         * (io-pgtable-arm.c:1263), then map()/unmap() publish every table. */
        CHECK(vm.prepare(&pages, 48, 40, false, &cache) == kIOReturnSuccess);
        CHECK(cache.calls.size() == 1);
        CHECK(cache.calls[0].cpu == pages.cpu[0]);
        CHECK(cache.calls[0].bytes == NativeMaliVmGranule);
        CHECK(cache.calls[0].operation == NativeMaliCacheClean);

        const uint64_t iova = UINT64_C(0x10000000);
        CHECK(vm.map(iova, UINT64_C(0x80000000), 8 * NativeMaliVmGranule,
            rw_cached) == kIOReturnSuccess);
        CHECK(vm.tables() == 4);
        CHECK(cache.calls.size() == 5);   /* root again, then the three new tables */
        CHECK(cache.calls[1].cpu == pages.cpu[0]);
        for(size_t i = 2; i < 5; ++i) {
            CHECK(cache.calls[i].cpu == pages.cpu[i - 1]);
            CHECK(cache.calls[i].bytes == NativeMaliVmGranule);
            CHECK(cache.calls[i].operation == NativeMaliCacheClean);
        }
        /* unmap() publishes the cleared entries on the same tables (empty
         * tables stay allocated here, unlike Linux). */
        CHECK(vm.unmap(iova, 8 * NativeMaliVmGranule) == kIOReturnSuccess);
        CHECK(cache.calls.size() == 9);
        for(size_t i = 5; i < 9; ++i)
            CHECK(cache.calls[i].operation == NativeMaliCacheClean);
        vm.reset();
        CHECK(pages.live == 0);

        /* A coherent walker never maintains anything. */
        cache.calls.clear();
        CHECK(vm.prepare(&pages, 48, 40, true, &cache) == kIOReturnSuccess);
        CHECK(vm.map(iova, UINT64_C(0x80000000), NativeMaliVmGranule, rw_cached)
            == kIOReturnSuccess);
        CHECK(vm.unmap(iova, NativeMaliVmGranule) == kIOReturnSuccess);
        CHECK(cache.calls.empty());
        vm.reset();
        CHECK(pages.live == 0);

        /* Without a maintainer the publish stays the caller's job, as before:
         * building tables is still allowed and nothing is maintained. */
        cache.calls.clear();
        CHECK(vm.prepare(&pages, 48, 40, false) == kIOReturnSuccess);
        CHECK(vm.map(iova, UINT64_C(0x80000000), NativeMaliVmGranule, rw_cached)
            == kIOReturnSuccess);
        CHECK(cache.calls.empty());
        vm.reset();
        CHECK(pages.live == 0);
    }

    /* ---- a failed publish is an error, not a silently private table ---- */
    {
        FakeCache cache;
        FakePages pages;
        NativeMaliVm vm;
        /* prepare() cannot publish the root: everything is torn down. */
        cache.result = kIOReturnNoResources;
        CHECK(vm.prepare(&pages, 48, 40, false, &cache) == kIOReturnNoResources);
        CHECK(!vm.prepared());
        CHECK(pages.live == 0);

        /* map() cannot publish the leaves: the range is taken back out and the
         * maintainer's error is returned unchanged. */
        cache.result = kIOReturnSuccess;
        CHECK(vm.prepare(&pages, 48, 40, false, &cache) == kIOReturnSuccess);
        cache.result = kIOReturnNoResources;
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0x80000000),
            NativeMaliVmGranule, rw_cached) == kIOReturnNoResources);
        CHECK(vm.mappedPages() == 0);
        uint64_t pte = UINT64_C(0xdeadbeef);
        CHECK(vm.descriptor(UINT64_C(0x10000000), &pte) == kIOReturnSuccess);
        CHECK(pte == 0);

        /* unmap() cannot publish the cleared entry: the leaves are clear, the
         * error is returned, and a retry publishes again. */
        cache.result = kIOReturnSuccess;
        CHECK(vm.map(UINT64_C(0x10000000), UINT64_C(0x80000000),
            NativeMaliVmGranule, rw_cached) == kIOReturnSuccess);
        const size_t calls = cache.calls.size();
        cache.result = kIOReturnNoResources;
        CHECK(vm.unmap(UINT64_C(0x10000000), NativeMaliVmGranule) ==
            kIOReturnNoResources);
        CHECK(vm.mappedPages() == 0);
        cache.result = kIOReturnSuccess;
        CHECK(vm.unmap(UINT64_C(0x10000000), NativeMaliVmGranule) ==
            kIOReturnSuccess);
        CHECK(cache.calls.size() > calls);
        vm.reset();
        CHECK(pages.live == 0);
    }

    std::printf("PASS: Mali LPAE geometry (ias 32/39/40/48), descriptor golden words, "
        "AS_MEMATTR 0x00888d88, AS_TRANSTAB bits, table sharing, refusals, the 64-table "
        "limit, the non-coherent page-table publish and a randomized map/walk/unmap sweep\n");
}
