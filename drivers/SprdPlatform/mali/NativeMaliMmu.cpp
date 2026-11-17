/* SPDX-License-Identifier: GPL-2.0-only */
/* AS slots and the MMU enable/disable sequence; sources are in NativeMaliMmu.h. */
#include "NativeMaliMmu.h"
#include "NativeMaliRegs.h"

namespace {

uint32_t lower32(uint64_t value)
{
    return static_cast<uint32_t>(value);
}

uint32_t upper32(uint64_t value)
{
    return static_cast<uint32_t>(value >> 32);
}

/* fls64(): 1-based position of the highest set bit, 0 for zero. */
uint32_t bitLength(uint64_t value)
{
    uint32_t length = 0;
    while(value) {
        ++length;
        value >>= 1;
    }
    return length;
}

/* ffs() for a non-zero mask. */
uint32_t firstSet(uint32_t value)
{
    uint32_t index = 0;
    while((value & 1u) == 0) {
        value >>= 1;
        ++index;
    }
    return index;
}

IOReturn readWord(NativeMaliRegisterTarget *target, uint32_t offset,
    uint32_t *value)
{
    if(!target || !value)return kIOReturnBadArgument;
    return target->read32(offset, value);
}

IOReturn writeWord(NativeMaliRegisterTarget *target, uint32_t offset,
    uint32_t value)
{
    if(!target)return kIOReturnBadArgument;
    return target->write32(offset, value);
}

} // namespace

bool NativeMaliMmu::lockRegionValue(uint64_t start, uint64_t size,
    uint64_t *value)
{
    if(!value || size == 0)return false;
    const uint64_t end = start + size;
    if(end < start)return false;   /* the source would compute on a wrapped end */

    /* max(fls64(start ^ (end - 1)), const_ilog2(AS_LOCK_REGION_MIN_SIZE)) - 1 */
    const uint32_t differing = bitLength(start ^ (end - 1));
    const uint32_t minimum = bitLength(AS_LOCK_REGION_MIN_SIZE) - 1;
    const uint32_t width = (differing > minimum ? differing : minimum) - 1;

    /* GENMASK_ULL(63, width), then region_width | region_start. */
    const uint64_t base = start & (~UINT64_C(0) << width);
    *value = base | width;
    return true;
}

IOReturn NativeMaliMmu::prepare(NativeMaliRegisterTarget *registers,
    NativeMaliPower *power)
{
    if(!registers || !power)return kIOReturnBadArgument;
    if(registers_ && (registers_ != registers || power_ != power))
        return kIOReturnBusy;
    if(!power->prepared())return kIOReturnNotReady;
    registers_ = registers;
    power_ = power;
    return kIOReturnSuccess;
}

IOReturn NativeMaliMmu::setPresent(uint32_t asPresent)
{
    if(!prepared())return kIOReturnNotReady;
    if(asPresent == 0)return kIOReturnBadArgument;
    if(asPresent_ != 0 && asPresent_ != asPresent)return kIOReturnBusy;
    asPresent_ = asPresent;
    return kIOReturnSuccess;
}

IOReturn NativeMaliMmu::allocate(uint32_t *as)
{
    if(!as)return kIOReturnBadArgument;
    if(asPresent_ == 0)return kIOReturnNotReady;
    const uint32_t freeSlots = asPresent_ & ~allocated_;
    if(freeSlots == 0)return kIOReturnNoResources;
    const uint32_t slot = firstSet(freeSlots);
    allocated_ |= 1u << slot;
    *as = slot;
    return kIOReturnSuccess;
}

IOReturn NativeMaliMmu::reclaim(uint32_t as)
{
    if(as >= NativeMaliMmuMaxAs || (asPresent_ & (1u << as)) == 0)
        return kIOReturnBadArgument;
    if(allocated_ & (1u << as))return kIOReturnBusy;
    allocated_ |= 1u << as;
    return kIOReturnSuccess;
}

IOReturn NativeMaliMmu::release(uint32_t as)
{
    if(as >= NativeMaliMmuMaxAs)return kIOReturnBadArgument;
    const uint32_t bit = 1u << as;
    if((allocated_ & bit) == 0)return kIOReturnBadArgument;
    allocated_ &= ~bit;
    return kIOReturnSuccess;
}

bool NativeMaliMmu::isAllocated(uint32_t as) const
{
    return as < NativeMaliMmuMaxAs && (allocated_ & (1u << as)) != 0;
}

void NativeMaliMmu::resetAllocation()
{
    allocated_ = 0;
}

IOReturn NativeMaliMmu::waitReady(uint32_t as, uint64_t timeout_us)
{
    uint32_t value = 0;
    uint64_t waited = 0;
    for(;;) {
        IOReturn result = readWord(registers_, AS_STATUS(as), &value);
        if(result != kIOReturnSuccess)return result;
        if((value & AS_STATUS_AS_ACTIVE) == 0)return kIOReturnSuccess;
        if(waited >= timeout_us)return kIOReturnTimeout;
        result = registers_->delay(ReadyPollUs);
        if(result != kIOReturnSuccess)return result;
        waited += ReadyPollUs;
    }
}

IOReturn NativeMaliMmu::writeCommand(uint32_t as, uint32_t command,
    uint64_t timeout_us)
{
    IOReturn result = waitReady(as, timeout_us);
    if(result != kIOReturnSuccess)return result;
    return writeWord(registers_, AS_COMMAND(as), command);
}

IOReturn NativeMaliMmu::operate(uint32_t as, uint64_t start, uint64_t size,
    uint32_t command, uint64_t timeout_us)
{
    uint64_t region = 0;
    if(!lockRegionValue(start, size, &region))return kIOReturnBadArgument;
    IOReturn result = writeWord(registers_, AS_LOCKADDR_LO(as),
        lower32(region));
    if(result != kIOReturnSuccess)return result;
    result = writeWord(registers_, AS_LOCKADDR_HI(as), upper32(region));
    if(result != kIOReturnSuccess)return result;
    result = writeCommand(as, AS_COMMAND_LOCK, timeout_us);
    if(result != kIOReturnSuccess)return result;
    result = writeCommand(as, command, timeout_us);
    if(result != kIOReturnSuccess)return result;
    return waitReady(as, timeout_us);
}

IOReturn NativeMaliMmu::writePair(uint32_t low, uint32_t high, uint64_t value)
{
    IOReturn result = writeWord(registers_, low, lower32(value));
    if(result != kIOReturnSuccess)return result;
    return writeWord(registers_, high, upper32(value));
}

IOReturn NativeMaliMmu::enable(uint32_t as, const NativeMaliVm &vm,
    uint64_t timeout_us)
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    if(as >= NativeMaliMmuMaxAs || (asPresent_ & (1u << as)) == 0)
        return kIOReturnBadArgument;
    if(!isAllocated(as))return kIOReturnBadArgument;
    if(!vm.prepared())return kIOReturnNotReady;

    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    const uint64_t transtab = vm.transtab();
    if((transtab & AS_TRANSTAB_LPAE_ADRMODE_MASK) !=
        AS_TRANSTAB_LPAE_ADRMODE_TABLE)
        return kIOReturnUnsupported;

    /* panfrost_mmu_enable(), panfrost_mmu.c:167-190. The whole-space FLUSH_MEM
     * locks 0..~0 first (mmu_hw_do_operation_locked, lines 139-152). */
    result = operate(as, 0, ~UINT64_C(0), AS_COMMAND_FLUSH_MEM, timeout_us);
    if(result != kIOReturnSuccess)return result;

    /* From here on a partial configuration exists, so every failure quiesces
     * the AS before returning the original error. */
    result = writePair(AS_TRANSTAB_LO(as), AS_TRANSTAB_HI(as), transtab);
    if(result != kIOReturnSuccess) {
        (void)disable(as, timeout_us);
        return result;
    }
    result = writePair(AS_MEMATTR_LO(as), AS_MEMATTR_HI(as), vm.memattr());
    if(result != kIOReturnSuccess) {
        (void)disable(as, timeout_us);
        return result;
    }
    result = writePair(AS_TRANSCFG_LO(as), AS_TRANSCFG_HI(as),
        AS_TRANSCFG_ADRMODE_LEGACY);
    if(result != kIOReturnSuccess) {
        (void)disable(as, timeout_us);
        return result;
    }
    result = writeCommand(as, AS_COMMAND_UPDATE, timeout_us);
    if(result != kIOReturnSuccess)(void)disable(as, timeout_us);
    return result;
}

IOReturn NativeMaliMmu::readFault(uint32_t as, NativeMaliMmuFault *fault)
{
    if(!fault)return kIOReturnBadArgument;
    *fault = NativeMaliMmuFault{};
    if(!prepared() || asPresent_ == 0)return kIOReturnNotReady;
    if(as >= NativeMaliMmuMaxAs || (asPresent_ & (1u << as)) == 0)
        return kIOReturnBadArgument;

    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    /* panfrost_mmu.c:897-899: the status, then the low word, then the high
     * word. */
    result = registers_->read32(AS_FAULTSTATUS(as), &fault->status);
    if(result != kIOReturnSuccess)return result;
    result = registers_->read32(AS_FAULTADDRESS_LO(as), &fault->addressLo);
    if(result != kIOReturnSuccess)return result;
    return registers_->read32(AS_FAULTADDRESS_HI(as), &fault->addressHi);
}

IOReturn NativeMaliMmu::flushRange(uint32_t as, uint64_t start, uint64_t size,
    uint64_t timeout_us)
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    if(as >= NativeMaliMmuMaxAs || (asPresent_ & (1u << as)) == 0)
        return kIOReturnBadArgument;
    if(!isAllocated(as))return kIOReturnBadArgument;
    if(!size)return kIOReturnBadArgument;

    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    /* panfrost_mmu_flush_range(): mmu_hw_do_operation_locked() with FLUSH_PT,
     * which locks the range first and waits for the flush afterwards. */
    return operate(as, start, size, AS_COMMAND_FLUSH_PT, timeout_us);
}

IOReturn NativeMaliMmu::disable(uint32_t as, uint64_t timeout_us)
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    if(as >= NativeMaliMmuMaxAs || (asPresent_ & (1u << as)) == 0)
        return kIOReturnBadArgument;

    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    /* panfrost_mmu_disable(), panfrost_mmu.c:191-205. */
    result = operate(as, 0, ~UINT64_C(0), AS_COMMAND_FLUSH_MEM, timeout_us);
    if(result != kIOReturnSuccess)return result;
    result = writePair(AS_TRANSTAB_LO(as), AS_TRANSTAB_HI(as), 0);
    if(result != kIOReturnSuccess)return result;
    result = writePair(AS_MEMATTR_LO(as), AS_MEMATTR_HI(as), 0);
    if(result != kIOReturnSuccess)return result;
    result = writePair(AS_TRANSCFG_LO(as), AS_TRANSCFG_HI(as),
        AS_TRANSCFG_ADRMODE_UNMAPPED);
    if(result != kIOReturnSuccess)return result;
    return writeCommand(as, AS_COMMAND_UPDATE, timeout_us);
}
