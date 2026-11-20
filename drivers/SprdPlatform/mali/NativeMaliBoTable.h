/* SPDX-License-Identifier: GPL-2.0-only */
/* One client's buffer objects: allocation, mapping into its one GPU address
 * space, the CPU token, and retirement.
 *
 * This is the state machine the IOUserClient will dispatch into. It is pure:
 * no IOKit object, no register of its own, no policy the wrapper would have to
 * repeat. Everything that touches hardware goes through the injected
 * NativeMaliMemorySource, NativeMaliVm and NativeMaliMmu, which is why the host
 * test can run the real page tables and the real LOCK/FLUSH_PT sequence against
 * fake registers.
 *
 * Shape (NativeAdrenoUserClient.h:29-46 and .cpp:121-324, MIT, see NOTICE; no
 * code copied):
 *   - a handle is this object's own identity, never a VM address, an IOVA or a
 *     pointer. It is handed out monotonically, so a stale handle from a freed
 *     object cannot name a newer one.
 *   - an object is retirable only through freeBo(), which unmaps first and then
 *     releases the backing memory; if either step fails the object stays live
 *     so the call can be repeated. That is also the order panfrost uses
 *     (panfrost_mmu.c:500-524 clears the leaves, then the GEM pages go).
 *   - a CPU mapping is a token, not a second handle: the wrapper's
 *     clientMemoryForType() maps the memory it names. A BO with a live token is
 *     not freed, because the wrapper still has it mapped.
 *   - teardown drops the CPU tokens first (the connection that held them is
 *     going away, as in NativeAdrenoUserClient::releaseAll()) and then frees
 *     each BO.
 *
 * Mapping permissions: the wire ABI requires read and lets the caller add
 * write (NativeMaliABI.h). panfrost always maps IOMMU_READ | IOMMU_WRITE |
 * IOMMU_CACHE (panfrost_mmu.c:466-493), so the read-only case is the only
 * difference here: it is honoured because the LPAE encoder can express it, and
 * a GPU write to such a mapping then faults instead of silently succeeding.
 *
 * Deliberately not here: choosing the address space (the client owns one, the
 * service allocates it), deciding which IOVA to use (the ABI's caller-chosen
 * address, as NativeMaliMemory::map() takes it), job submission and fences
 * (NativeMaliJobSlot), and any call into XNU.
 */
#ifndef NATIVE_MALI_BO_TABLE_H
#define NATIVE_MALI_BO_TABLE_H
#include "NativeMaliABI.h"
#include "NativeMaliAbiValidate.h"
#include "NativeMaliHandleIndex.h"
#include "NativeMaliMemory.h"
#include "NativeMaliMmu.h"
#include "NativeMaliVm.h"
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

/* Live objects per client. The table is fixed storage; alloc() reports
 * kIOReturnNoResources past it rather than growing. */
enum : size_t { NativeMaliBoTableCapacity = 64 };

class NativeMaliBoTable final {
    struct Slot {
        NativeMaliMemory memory;
        uint64_t handle = 0;
        uint64_t iova = 0;
        uint32_t permissions = 0;
        uint32_t cpuToken = 0;
        bool live = false;
        bool mapped = false;
        bool cpuMapped = false;
        bool imported = false;
    };
    Slot slot_[NativeMaliBoTableCapacity];
    NativeMaliHandleIndex<NativeMaliBoTableCapacity> index_; /* handle -> slot */
    size_t live_ = 0;
    uint64_t nextHandle_ = 1;

    NativeMaliMemorySource *source_ = nullptr;
    NativeMaliCacheMaintainer *maintainer_ = nullptr;
    NativeMaliVm *vm_ = nullptr;
    NativeMaliMmu *mmu_ = nullptr;
    uint32_t as_ = 0;
    uint64_t maxBytes_ = 0;
    bool coherent_ = false;
    bool open_ = false;

    Slot *find(uint64_t handle);
    const Slot *find(uint64_t handle) const;
    Slot *freeSlot();
    bool rangeTaken(uint64_t iova, uint64_t bytes) const;
    void clear(Slot &slot);
    IOReturn create(uint64_t bytes, uint64_t address,
        NativeMaliAbiBoAllocReply *reply);
public:
    NativeMaliBoTable() = default;
    /* A destroyed table only drops CPU-side state; the address space must
     * already be gone (NativeMaliMemory::~NativeMaliMemory). */
    ~NativeMaliBoTable();
    NativeMaliBoTable(const NativeMaliBoTable &) = delete;
    NativeMaliBoTable &operator=(const NativeMaliBoTable &) = delete;

    /* Bind the client's one address space. `maxBytes` is what Query advertises
     * (0 means NATIVE_MALI_ABI_MAX_BO_BYTES). A non-coherent buffer needs a
     * maintainer, exactly as NativeMaliMemory::prepare() requires; refusing
     * here says so once instead of on every alloc(). The address space must be
     * present and allocated, because every map ends in its FLUSH_PT. */
    IOReturn open(NativeMaliMemorySource *source,
        NativeMaliCacheMaintainer *maintainer, bool cpuCoherent,
        NativeMaliVm *vm, NativeMaliMmu *mmu, uint32_t as, uint64_t maxBytes);
    bool open() const { return open_; }

    /* The ABI entry points. Each validates its own record through
     * NativeMaliAbiValidate, then applies what only this table knows: does the
     * handle exist, is it already mapped, is the range already taken, does the
     * token match. Every reply is zeroed before a refusal can return. */
    IOReturn alloc(const NativeMaliAbiBoAlloc &request,
        NativeMaliAbiBoAllocReply *reply);
    IOReturn importUser(const NativeMaliAbiBoImportUser &request,
        NativeMaliAbiBoAllocReply *reply);
    bool hasImported() const;
    bool imported(uint64_t handle) const;
    IOReturn map(const NativeMaliAbiBoMap &request,
        NativeMaliAbiBoMapReply *reply);
    IOReturn freeBo(const NativeMaliAbiBoFree &request);
    IOReturn cpuMap(const NativeMaliAbiBoCpuMap &request,
        NativeMaliAbiBoCpuMapReply *reply);
    IOReturn cpuUnmap(const NativeMaliAbiBoCpuUnmap &request);

    /* Connection teardown: drop every CPU token, then free every live object in
     * slot order. The first failure stops the walk and leaves the rest live;
     * nothing is forced, so the caller can retry or report it. */
    IOReturn releaseAll();
    /* After the address space is gone: give every allocation back and forget
     * the handles without touching a register. Cannot fail, cannot be retried
     * into a different state. */
    void reset();

    size_t live() const { return live_; }
    /* True when [start, start + bytes) lies inside one live mapping. A job
     * chain head is only a GPU address; this is how the submission path checks
     * that the address is one this client actually mapped. */
    bool covers(uint64_t start, uint64_t bytes) const;
    IOReturn readMapped(uint64_t address, void *output, size_t bytes) const;
    IOReturn maintainMapped(bool forDevice);
    bool mapped(uint64_t handle) const;
    bool coherent() const { return coherent_; }
    /* The backing object of a live handle, for the wrapper and for kernel
     * self-tests; null for an unknown handle. The table keeps ownership. */
    NativeMaliMemory *memory(uint64_t handle);
    const NativeMaliMemory *memory(uint64_t handle) const;
    /* The object a BoCpuMap token names while it is held; null otherwise. */
    NativeMaliMemory *memoryForToken(uint32_t token);
};

#endif
