/* SPDX-License-Identifier: GPL-2.0-only */
/* The client's BO state machine; the rules and their sources are in
 * NativeMaliBoTable.h. */
#include "NativeMaliBoTable.h"
#include <string.h>

NativeMaliBoTable::~NativeMaliBoTable()
{
    /* The slots' NativeMaliMemory destructors give the allocations back; the
     * handle index is the only heap this object owns. */
    index_.release();
}

NativeMaliBoTable::Slot *NativeMaliBoTable::find(uint64_t handle)
{
    if(!open_ || !handle)return nullptr;
    const uint32_t slot = index_.find(handle);
    if(slot >= NativeMaliBoTableCapacity)return nullptr;
    Slot &found = slot_[slot];
    /* The index is the fast path; the handle on the slot is the truth. A
     * disagreement means a bug, and the answer is "unknown", never a stale
     * object. */
    if(!found.live || found.handle != handle)return nullptr;
    return &found;
}

const NativeMaliBoTable::Slot *NativeMaliBoTable::find(uint64_t handle) const
{
    return const_cast<NativeMaliBoTable *>(this)->find(handle);
}

NativeMaliBoTable::Slot *NativeMaliBoTable::freeSlot()
{
    for(size_t i = 0; i < NativeMaliBoTableCapacity; ++i)
        if(!slot_[i].live)return &slot_[i];
    return nullptr;
}

/* [iova, iova + bytes) against every live mapping. The records are validated
 * first, so neither end can wrap here. */
bool NativeMaliBoTable::rangeTaken(uint64_t iova, uint64_t bytes) const
{
    for(size_t i = 0; i < NativeMaliBoTableCapacity; ++i) {
        const Slot &slot = slot_[i];
        if(!slot.live || !slot.mapped)continue;
        const uint64_t start = slot.memory.iova();
        const uint64_t end = start + slot.memory.bytes();
        if(iova < end && start < iova + bytes)return true;
    }
    return false;
}

void NativeMaliBoTable::clear(Slot &slot)
{
    index_.erase(slot.handle);
    if(slot.live && live_)--live_;
    slot.handle = 0;
    slot.iova = 0;
    slot.permissions = 0;
    slot.cpuToken = 0;
    slot.live = false;
    slot.mapped = false;
    slot.cpuMapped = false;
}

IOReturn NativeMaliBoTable::open(NativeMaliMemorySource *source,
    NativeMaliCacheMaintainer *maintainer, bool cpuCoherent,
    NativeMaliVm *vm, NativeMaliMmu *mmu, uint32_t as, uint64_t maxBytes)
{
    if(open_)return kIOReturnBusy;
    if(!source || !vm || !mmu || as >= NativeMaliMmuMaxAs)return kIOReturnBadArgument;
    /* The clean/invalidate policy is not optional on this non-coherent GPU
     * (NativeMaliMemory.h); say so once instead of on every alloc(). */
    if(!cpuCoherent && !maintainer)return kIOReturnBadArgument;
    /* A map ends in FLUSH_PT on this AS, which a slot that is not present and
     * allocated cannot answer. */
    if((mmu->present() & (1u << as)) == 0 || !mmu->isAllocated(as))
        return kIOReturnBadArgument;

    if(!index_.allocate())return kIOReturnNoMemory;

    source_ = source;
    maintainer_ = maintainer;
    vm_ = vm;
    mmu_ = mmu;
    as_ = as;
    maxBytes_ = maxBytes ? maxBytes :
        static_cast<uint64_t>(NATIVE_MALI_ABI_MAX_BO_BYTES);
    coherent_ = cpuCoherent;
    open_ = true;
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::alloc(const NativeMaliAbiBoAlloc &request,
    NativeMaliAbiBoAllocReply *reply)
{
    if(!reply)return kIOReturnBadArgument;
    reply->handle = 0;
    if(!open_)return kIOReturnNotReady;

    IOReturn result = NativeMaliAbiValidate::boAlloc(request, maxBytes_);
    if(result != kIOReturnSuccess)return result;

    Slot *slot = freeSlot();
    if(!slot)return kIOReturnNoResources;

    result = slot->memory.prepare(source_, static_cast<size_t>(request.bytes),
        coherent_, maintainer_);
    if(result != kIOReturnSuccess)return result;

    /* Monotonic, never 0: a stale handle cannot name a later object. */
    uint64_t handle = nextHandle_++;
    if(nextHandle_ == 0)++nextHandle_;
    if(handle == 0)handle = nextHandle_++;

    slot->handle = handle;
    slot->iova = 0;
    slot->permissions = 0;
    slot->cpuToken = 0;
    slot->live = true;
    slot->mapped = false;
    slot->cpuMapped = false;
    index_.insert(handle, static_cast<uint32_t>(slot - slot_));
    ++live_;
    reply->handle = handle;
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::map(const NativeMaliAbiBoMap &request,
    NativeMaliAbiBoMapReply *reply)
{
    if(!reply)return kIOReturnBadArgument;
    reply->iova = 0;
    if(!open_)return kIOReturnNotReady;

    Slot *slot = find(request.handle);
    if(!slot)return kIOReturnBadArgument;
    IOReturn result = NativeMaliAbiValidate::boMap(request, slot->memory.bytes());
    if(result != kIOReturnSuccess)return result;
    if(slot->mapped)return kIOReturnBusy;
    /* Refuse before the first leaf is written: a partial overlap would leave
     * the new object half-mapped and the old one partly shadowed. */
    if(rangeTaken(request.iova, slot->memory.bytes()))return kIOReturnBadArgument;

    uint32_t prot = NativeMaliVmRead | NativeMaliVmCached;
    if(request.permissions & NativeMaliBoWrite)prot |= NativeMaliVmWrite;

    result = slot->memory.map(vm_, mmu_, as_, request.iova, prot);
    if(result != kIOReturnSuccess)return result;

    slot->mapped = true;
    slot->iova = request.iova;
    slot->permissions = request.permissions;
    reply->iova = request.iova;
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::freeBo(const NativeMaliAbiBoFree &request)
{
    if(!open_)return kIOReturnNotReady;
    IOReturn result = NativeMaliAbiValidate::boFree(request);
    if(result != kIOReturnSuccess)return result;

    Slot *slot = find(request.handle);
    if(!slot)return kIOReturnBadArgument;
    /* The wrapper still has this memory mapped; freeing it now would leave the
     * mapping pointing at pages the source already took back. */
    if(slot->cpuMapped)return kIOReturnBusy;

    /* Leaves first, then the pages (panfrost_mmu.c:500-524). A failed unmap
     * keeps the object live and mapped, so the call can be repeated. */
    if(slot->mapped) {
        result = slot->memory.unmap();
        if(result != kIOReturnSuccess)return result;
        slot->mapped = false;
        slot->iova = 0;
        slot->permissions = 0;
    }

    result = slot->memory.release();
    if(result != kIOReturnSuccess)return result;

    clear(*slot);
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::cpuMap(const NativeMaliAbiBoCpuMap &request,
    NativeMaliAbiBoCpuMapReply *reply)
{
    if(!reply)return kIOReturnBadArgument;
    *reply = NativeMaliAbiBoCpuMapReply{};
    if(!open_)return kIOReturnNotReady;

    IOReturn result = NativeMaliAbiValidate::boCpuMap(request);
    if(result != kIOReturnSuccess)return result;

    Slot *slot = find(request.handle);
    if(!slot)return kIOReturnBadArgument;
    if(slot->cpuMapped)return kIOReturnBusy;

    /* Slot + 1: nonzero, and unique among the live tokens of this table. The
     * handle in cpuUnmap() is what keeps a stale token from naming a newer
     * object that landed in the same slot. */
    slot->cpuToken = static_cast<uint32_t>(slot - slot_) + 1;
    slot->cpuMapped = true;
    reply->token = slot->cpuToken;
    reply->bytes = slot->memory.bytes();
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::cpuUnmap(const NativeMaliAbiBoCpuUnmap &request)
{
    if(!open_)return kIOReturnNotReady;
    IOReturn result = NativeMaliAbiValidate::boCpuUnmap(request);
    if(result != kIOReturnSuccess)return result;

    Slot *slot = find(request.handle);
    if(!slot)return kIOReturnBadArgument;
    /* A token of 0 is never handed out, so "not mapped" and "wrong token" are
     * both refusals rather than a silent no-op. */
    if(!slot->cpuMapped || slot->cpuToken != request.token)
        return kIOReturnBadArgument;

    slot->cpuMapped = false;
    slot->cpuToken = 0;
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoTable::releaseAll()
{
    if(!open_)return kIOReturnNotReady;

    /* The connection that held these tokens is closing, so they cannot be
     * returned to their owner (NativeAdrenoUserClient::releaseAll() clears its
     * leases for the same reason). Drop every token first, then retire. */
    for(Slot &slot : slot_) {
        slot.cpuMapped = false;
        slot.cpuToken = 0;
    }

    for(size_t i = 0; i < NativeMaliBoTableCapacity; ++i) {
        if(!slot_[i].live)continue;
        NativeMaliAbiBoFree request{};
        request.handle = slot_[i].handle;
        const IOReturn result = freeBo(request);
        if(result != kIOReturnSuccess)return result;
    }
    return kIOReturnSuccess;
}

void NativeMaliBoTable::reset()
{
    for(Slot &slot : slot_) {
        /* No registers: the caller promises the address space is already
         * gone, so a still-mapped object only has its CPU side dropped. */
        slot.memory.reset();
        slot.handle = 0;
        slot.iova = 0;
        slot.permissions = 0;
        slot.cpuToken = 0;
        slot.live = false;
        slot.mapped = false;
        slot.cpuMapped = false;
    }
    index_.release();
    live_ = 0;
    open_ = false;
}

bool NativeMaliBoTable::covers(uint64_t start, uint64_t bytes) const
{
    if(!bytes)return false;
    for(const Slot &slot : slot_) {
        if(!slot.live || !slot.mapped)continue;
        const uint64_t begin = slot.memory.iova();
        const uint64_t size = slot.memory.bytes();
        /* Subtraction form: `begin + size` is inside the 48-bit space the map
         * was validated against, but `start + bytes` is the caller's and may
         * wrap. */
        if(start < begin || start - begin > size)continue;
        if(bytes <= size - (start - begin))return true;
    }
    return false;
}

bool NativeMaliBoTable::mapped(uint64_t handle) const
{
    const Slot *slot = find(handle);
    return slot && slot->mapped;
}

NativeMaliMemory *NativeMaliBoTable::memory(uint64_t handle)
{
    Slot *slot = find(handle);
    return slot ? &slot->memory : nullptr;
}

const NativeMaliMemory *NativeMaliBoTable::memory(uint64_t handle) const
{
    const Slot *slot = find(handle);
    return slot ? &slot->memory : nullptr;
}

NativeMaliMemory *NativeMaliBoTable::memoryForToken(uint32_t token)
{
    if(!open_ || !token)return nullptr;
    for(Slot &slot : slot_)
        if(slot.live && slot.cpuMapped && slot.cpuToken == token)
            return &slot.memory;
    return nullptr;
}

IOReturn NativeMaliBoTable::readMapped(uint64_t address, void *output, size_t bytes) const
{
    if(!open_ || !output || !bytes)return kIOReturnBadArgument;
    for(const auto &slot : slot_) {
        if(!slot.live || !slot.mapped || address < slot.memory.iova())continue;
        const uint64_t offset = address - slot.memory.iova();
        if(offset <= slot.memory.bytes() && bytes <= slot.memory.bytes() - offset) {
            memcpy(output, static_cast<const uint8_t *>(slot.memory.cpu()) + offset, bytes);
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

IOReturn NativeMaliBoTable::maintainMapped(bool forDevice)
{
    if(!open_)return kIOReturnNotReady;
    for(auto &slot : slot_) {
        if(!slot.live || !slot.mapped)continue;
        // A GPU read-only PTE cannot have made the CPU's copy stale.
        if(!forDevice && !(slot.permissions & NativeMaliBoWrite))continue;
        const IOReturn result = forDevice
            ? slot.memory.cleanForDevice(0, slot.memory.bytes())
            : slot.memory.invalidateForCpu(0, slot.memory.bytes());
        if(result != kIOReturnSuccess)return result;
    }
    return kIOReturnSuccess;
}
