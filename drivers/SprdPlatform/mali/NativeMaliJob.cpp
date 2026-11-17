/* SPDX-License-Identifier: GPL-2.0-only */
/* The job-header and WRITE_VALUE packing; the pinned bit positions are in
 * NativeMaliJob.h. */
#include "NativeMaliJob.h"
#include <string.h>

namespace {

/* Word indices from v7.xml: the header is words 0-7, the WRITE_VALUE payload
 * starts at byte 32, so its Address is words 8-9, Type word 10 and Immediate
 * Value words 12-13 (word 11 is the unused word 3 of the payload struct). */
enum : size_t {
    WordExceptionStatus = 0,
    WordFirstIncompleteTask = 1,
    WordFaultPointerLo = 2,
    WordFaultPointerHi = 3,
    WordFlags = 4,
    WordDependencies = 5,
    WordNextLo = 6,
    WordNextHi = 7,
    WordAddressLo = 8,
    WordAddressHi = 9,
    WordWriteValueType = 10,
    WordImmediateLo = 12,
    WordImmediateHi = 13,
    WordsPerSlot = size_t(NativeMaliJobChain::SlotBytes) / 4
};

uint32_t *wordsOf(uint8_t *slot)
{
    return reinterpret_cast<uint32_t *>(slot);
}

const uint32_t *wordsOf(const uint8_t *slot)
{
    return reinterpret_cast<const uint32_t *>(slot);
}

} // namespace

uint8_t *NativeMaliJobChain::slot(size_t index) const
{
    return cpu_ + index * SlotBytes;
}

IOReturn NativeMaliJobChain::prepare(void *cpu, uint64_t gpu, size_t bytes)
{
    if(prepared_)return kIOReturnBusy;
    if(!cpu || !bytes)return kIOReturnBadArgument;
    if((reinterpret_cast<uintptr_t>(cpu) & (SlotBytes - 1)) != 0)
        return kIOReturnBadArgument;
    if((gpu & (SlotBytes - 1)) != 0)return kIOReturnBadArgument;
    if(bytes % SlotBytes)return kIOReturnBadArgument;
    if(gpu + bytes < gpu)return kIOReturnBadArgument;

    cpu_ = static_cast<uint8_t *>(cpu);
    gpu_ = gpu;
    slots_ = bytes / SlotBytes;
    jobs_ = 0;
    prepared_ = true;
    return kIOReturnSuccess;
}

void NativeMaliJobChain::reset()
{
    cpu_ = nullptr;
    gpu_ = 0;
    slots_ = 0;
    jobs_ = 0;
    prepared_ = false;
}

void NativeMaliJobChain::packHeader(uint8_t *slot,
    const NativeMaliJobHeader &header)
{
    uint32_t words[WordsPerSlot]{};
    words[WordExceptionStatus] = header.exceptionStatus;
    words[WordFirstIncompleteTask] = header.firstIncompleteTask;
    words[WordFaultPointerLo] = uint32_t(header.faultPointer);
    words[WordFaultPointerHi] = uint32_t(header.faultPointer >> 32);

    uint32_t flags = 0;
    if(header.is64Bit)flags |= 1u << 0;
    flags |= (header.type & 0x7F) << 1;
    if(header.barrier)flags |= 1u << 8;
    if(header.invalidateCache)flags |= 1u << 9;
    if(header.suppressPrefetch)flags |= 1u << 11;
    if(header.enableTextureMapper)flags |= 1u << 12;
    if(header.relaxDependency1)flags |= 1u << 14;
    if(header.relaxDependency2)flags |= 1u << 15;
    flags |= (header.index & 0xFFFF) << 16;
    words[WordFlags] = flags;
    words[WordDependencies] = (header.dependency1 & 0xFFFF) |
        ((header.dependency2 & 0xFFFF) << 16);
    words[WordNextLo] = uint32_t(header.next);
    words[WordNextHi] = uint32_t(header.next >> 32);

    /* The whole slot, so the payload area and the padding are zero too. */
    memcpy(slot, words, sizeof(words));
}

IOReturn NativeMaliJobChain::unpackHeader(const uint8_t *slot,
    NativeMaliJobHeader *header)
{
    if(!slot || !header)return kIOReturnBadArgument;
    const uint32_t *words = wordsOf(slot);
    header->exceptionStatus = words[WordExceptionStatus];
    header->firstIncompleteTask = words[WordFirstIncompleteTask];
    header->faultPointer = (static_cast<uint64_t>(words[WordFaultPointerHi]) << 32) |
        words[WordFaultPointerLo];
    const uint32_t flags = words[WordFlags];
    header->is64Bit = (flags & (1u << 0)) != 0;
    header->type = (flags >> 1) & 0x7F;
    header->barrier = (flags & (1u << 8)) != 0;
    header->invalidateCache = (flags & (1u << 9)) != 0;
    header->suppressPrefetch = (flags & (1u << 11)) != 0;
    header->enableTextureMapper = (flags & (1u << 12)) != 0;
    header->relaxDependency1 = (flags & (1u << 14)) != 0;
    header->relaxDependency2 = (flags & (1u << 15)) != 0;
    header->index = flags >> 16;
    header->dependency1 = words[WordDependencies] & 0xFFFF;
    header->dependency2 = words[WordDependencies] >> 16;
    header->next = (static_cast<uint64_t>(words[WordNextHi]) << 32) |
        words[WordNextLo];
    return kIOReturnSuccess;
}

IOReturn NativeMaliJobChain::addWriteValue(uint32_t writeValueType,
    uint64_t target, uint64_t immediate, uint32_t *index)
{
    if(index)*index = 0;
    if(!prepared_)return kIOReturnNotReady;
    /* An out-of-range type is an argument error whether or not the chain is
     * full, so it is checked before the capacity. */
    if(writeValueType < NativeMaliWriteValueCycleCounter ||
        writeValueType > NativeMaliWriteValueImmediate64)
        return kIOReturnBadArgument;
    if(jobs_ >= slots_)return kIOReturnNoResources;
    /* The Index field is 16 bits (v7.xml:1347), so job 65536 has no name and a
     * longer chain would silently truncate it. */
    if(jobs_ >= 0xFFFF)return kIOReturnNoResources;

    NativeMaliJobHeader header;
    header.type = NativeMaliJobWriteValue;
    header.is64Bit = true;   /* v7.xml's default, and this chain is 64-bit */
    header.index = uint32_t(jobs_ + 1);   /* pan_jc_add_job(): ++job_index */
    header.dependency1 = 0;
    header.dependency2 = 0;
    header.next = 0;
    uint8_t *created = slot(jobs_);
    packHeader(created, header);

    uint32_t *words = wordsOf(created);
    words[WordAddressLo] = uint32_t(target);
    words[WordAddressHi] = uint32_t(target >> 32);
    words[WordWriteValueType] = writeValueType;
    /* pan_jc_initialize_tiler() sets only the address and the type; every type
     * but the Immediate ones leaves the field at 0. */
    const bool immediateType = writeValueType >= NativeMaliWriteValueImmediate8;
    words[WordImmediateLo] = immediateType ? uint32_t(immediate) : 0;
    words[WordImmediateHi] = immediateType ? uint32_t(immediate >> 32) : 0;

    /* The new slot's own GPU address, before jobs_ is advanced (jobAt() only
     * answers for slots that already exist). */
    const uint64_t createdGpu = gpu_ + jobs_ * SlotBytes;
    if(jobs_) {
        /* Form the chain: the previous job's Next is patched in place, exactly
         * as pan_jc_add_job() writes opaque[6]/opaque[7]. */
        uint32_t *previous = wordsOf(slot(jobs_ - 1));
        previous[WordNextLo] = uint32_t(createdGpu);
        previous[WordNextHi] = uint32_t(createdGpu >> 32);
    }

    ++jobs_;
    if(index)*index = header.index;
    return kIOReturnSuccess;
}
