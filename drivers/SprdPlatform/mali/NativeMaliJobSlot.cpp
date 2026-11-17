/* SPDX-License-Identifier: GPL-2.0-only */
/* The job-slot submit and completion handshakes; every bit and order is cited
 * in NativeMaliJobSlot.h. */
#include "NativeMaliJobSlot.h"
#include "NativeMaliRegs.h"

IOReturn NativeMaliJobSlot::prepare(NativeMaliRegisterTarget *registers,
    NativeMaliPower *power, NativeMaliMmu *mmu)
{
    if(!registers || !power || !mmu)return kIOReturnBadArgument;
    if(registers_ && (registers_ != registers || power_ != power || mmu_ != mmu))
        return kIOReturnBusy;
    if(!power->prepared() || !mmu->prepared())return kIOReturnNotReady;
    registers_ = registers;
    power_ = power;
    mmu_ = mmu;
    return kIOReturnSuccess;
}

IOReturn NativeMaliJobSlot::setPresent(uint32_t jsPresent,
    uint64_t shaderPresent)
{
    if(!prepared())return kIOReturnNotReady;
    if(jsPresent == 0 || shaderPresent == 0)return kIOReturnBadArgument;
    if(jsPresent_ != 0 &&
        (jsPresent_ != jsPresent || shaderPresent_ != shaderPresent))
        return kIOReturnBusy;
    jsPresent_ = jsPresent;
    shaderPresent_ = shaderPresent;
    return kIOReturnSuccess;
}

uint32_t NativeMaliJobSlot::configValue(uint32_t as, bool jobChain)
{
    /* cfg = as_nr first (panfrost_job.c:233 "cfg = ret", printed as cfg & 0xf),
     * then the flags the source always sets for a v7 job. */
    uint32_t config = as;
    config |= JS_CONFIG_THREAD_PRI(8);
    config |= JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE;
    config |= JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE;
    /* HW_FEATURE_FLUSH_REDUCTION is in hw_features_g52, so the source both sets
     * this bit and writes JS_FLUSH_ID_NEXT (panfrost_job.c:242-252). */
    config |= JS_CONFIG_ENABLE_FLUSH_REDUCTION;
    /* JS_CONFIG_START_MMU needs HW_ISSUE_10649, which hw_issues_g52 (0) does
     * not have. */
    if(jobChain)config |= JS_CONFIG_JOB_CHAIN_FLAG;
    return config;
}

IOReturn NativeMaliJobSlot::writePair(uint32_t low, uint32_t high,
    uint64_t value)
{
    IOReturn result = registers_->write32(low, uint32_t(value));
    if(result != kIOReturnSuccess)return result;
    return registers_->write32(high, uint32_t(value >> 32));
}

IOReturn NativeMaliJobSlot::writeSequence(uint32_t js, uint64_t head,
    uint64_t affinity, uint32_t config, uint32_t flushId)
{
    /* panfrost_job.c:230-231 head, :233 affinity (via
     * panfrost_job_write_affinity), :248 config, :251 flush id, :270 GO. */
    IOReturn result = writePair(JS_HEAD_NEXT_LO(js), JS_HEAD_NEXT_HI(js), head);
    if(result != kIOReturnSuccess)return result;
    result = writePair(JS_AFFINITY_NEXT_LO(js), JS_AFFINITY_NEXT_HI(js),
        affinity);
    if(result != kIOReturnSuccess)return result;
    result = registers_->write32(JS_CONFIG_NEXT(js), config);
    if(result != kIOReturnSuccess)return result;
    result = registers_->write32(JS_FLUSH_ID_NEXT(js), flushId);
    if(result != kIOReturnSuccess)return result;
    return registers_->write32(JS_COMMAND_NEXT(js), JS_COMMAND_START);
}

IOReturn NativeMaliJobSlot::submit(uint32_t js, uint64_t head, uint32_t as,
    uint32_t flushId, NativeMaliFence *fence)
{
    if(fence)*fence = NativeMaliFence{};
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    if(jsPresent_ == 0 || shaderPresent_ == 0)return kIOReturnNotReady;
    if(js >= NativeMaliJobSlotCount || (jsPresent_ & (1u << js)) == 0)
        return kIOReturnBadArgument;
    /* One job per slot: the DONE bit carries no job identity, so a second job
     * in flight would make waitForCompletion() ambiguous. */
    if(outstanding_[js])return kIOReturnBusy;
    if(faulted_[js])return kIOReturnNotReady;
    if(as > NativeMaliJobMaxAddressSpace || !mmu_->isAllocated(as))
        return kIOReturnBadArgument;
    /* 0 is not a chain: NativeMaliJobChain::firstJob() returns 0 for an empty
     * one, and a GPU that walks IOVA 0 would fault (or worse, read whatever the
     * lowest mapping is). The 64-byte rule is v7.xml's align on the header. */
    if(!head || head % NativeMaliJobHeadAlignment)return kIOReturnBadArgument;

    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    /* panfrost_job.c:217: WARN_ON(job_read(pfdev, JS_COMMAND_NEXT(js))). A
     * pending command means this slot was not drained, so starting a chain
     * would overwrite it. */
    uint32_t staged = 0;
    result = registers_->read32(JS_COMMAND_NEXT(js), &staged);
    if(result != kIOReturnSuccess)return result;
    if(staged)return kIOReturnBusy;

    /* panfrost_jm_reset_interrupts() (panfrost_job.c:435-438) clears every job
     * event after a GPU reset, because the JM can latch one that no job of ours
     * produced. softReset() only clears GPU_INT_*, so the job palette is done
     * here instead: with outstanding_ false, any DONE/ERR bit of this slot is
     * stale, and leaving it would let the next wait return before the job ran. */
    const uint32_t own = JOB_INT_MASK_DONE(js) | JOB_INT_MASK_ERR(js);
    uint32_t latched = 0;
    result = registers_->read32(JOB_INT_RAWSTAT, &latched);
    if(result != kIOReturnSuccess)return result;
    if(latched & own) {
        result = registers_->write32(JOB_INT_CLEAR, latched & own);
        if(result != kIOReturnSuccess)return result;
    }

    const uint32_t seqno = emitSeqno_[js] + 1;
    result = writeSequence(js, head, shaderPresent_,
        configValue(as, (seqno & 1) != 0), flushId);
    if(result != kIOReturnSuccess) {
        /* panfrost_job.c:549 cancels a staged submission this way; whatever the
         * failed sequence left behind must not start later. The original error
         * is what the caller sees. */
        (void)registers_->write32(JS_COMMAND_NEXT(js), JS_COMMAND_NOP);
        return result;
    }
    emitSeqno_[js] = seqno;
    outstanding_[js] = true;
    if(fence)*fence = NativeMaliFence{js, seqno};
    return kIOReturnSuccess;
}

IOReturn NativeMaliJobSlot::waitForCompletion(uint32_t js,
    NativeMaliJobCompletion *completion, uint64_t timeout_us, uint64_t poll_us)
{
    if(!completion)return kIOReturnBadArgument;
    *completion = NativeMaliJobCompletion{};
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    if(jsPresent_ == 0 || shaderPresent_ == 0)return kIOReturnNotReady;
    if(js >= NativeMaliJobSlotCount || (jsPresent_ & (1u << js)) == 0)
        return kIOReturnBadArgument;
    if(!outstanding_[js])return kIOReturnNotReady;

    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    const uint32_t doneBit = JOB_INT_MASK_DONE(js);
    const uint32_t errorBit = JOB_INT_MASK_ERR(js);
    uint32_t status = 0;
    uint64_t waited = 0;
    for(;;) {
        result = registers_->read32(JOB_INT_RAWSTAT, &status);
        if(result != kIOReturnSuccess)return result;
        if(status & (doneBit | errorBit))break;
        if(waited >= timeout_us)return kIOReturnTimeout;
        result = registers_->delay(poll_us);
        if(result != kIOReturnSuccess)return result;
        waited += poll_us;
    }
    completion->rawStatus = status;

    if(status & errorBit) {
        /* panfrost_job_handle_err() (panfrost_job.c:454-470) reads JS_STATUS for
         * the reason, and panfrost_jm_handle_irq() (panfrost_job.c:544-550)
         * cancels the staged next command. Both are repeated by a retry if one
         * of them fails, which is why outstanding_ is only cleared at the end.
         * STOPPED (a resumable stop the source restarts from JS_TAIL) is
         * reported, not resumed: this driver has no job object to rewind and no
         * reset path to call yet, so the slot is latched instead. */
        result = registers_->read32(JS_STATUS(js), &completion->jsStatus);
        if(result != kIOReturnSuccess)return result;
        result = registers_->write32(JS_COMMAND_NEXT(js), JS_COMMAND_NOP);
        if(result != kIOReturnSuccess)return result;
        result = registers_->write32(JOB_INT_CLEAR, status & (doneBit | errorBit));
        if(result != kIOReturnSuccess)return result;
        outstanding_[js] = false;
        faulted_[js] = true;
        /* A faulted job still reaps its fence: the wait is over, the reason is
         * in the completion. */
        completedSeqno_[js] = emitSeqno_[js];
        return kIOReturnIOError;
    }

    /* DONE: clear only this slot's bit. The source clears the whole word it
     * read, but it samples JS_STATE under that write and owns every slot;
     * another slot's event is somebody else's completion here. */
    result = registers_->write32(JOB_INT_CLEAR, doneBit);
    if(result != kIOReturnSuccess)return result;
    outstanding_[js] = false;
    completedSeqno_[js] = emitSeqno_[js];
    return kIOReturnSuccess;
}

IOReturn NativeMaliJobSlot::resetInterrupts()
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    /* Same rule as submit(): do not touch a job register before GPU_JS_PRESENT
     * is known. The source's call site is after panfrost_jm_init(). */
    if(jsPresent_ == 0)return kIOReturnNotReady;
    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;
    return registers_->write32(JOB_INT_CLEAR, NativeMaliAllJobInterrupts);
}

IOReturn NativeMaliJobSlot::quiesce()
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    if(jsPresent_ == 0)return kIOReturnNotReady;
    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    /* panfrost_job.c:697-701: cancel the staged next command, then soft-stop the
     * running one, in that order on each slot. */
    for(uint32_t js = 0; js < NativeMaliJobSlotCount; ++js) {
        if((jsPresent_ & (1u << js)) == 0)continue;
        result = registers_->write32(JS_COMMAND_NEXT(js), JS_COMMAND_NOP);
        if(result != kIOReturnSuccess)return result;
        result = registers_->write32(JS_COMMAND(js), JS_COMMAND_SOFT_STOP);
        if(result != kIOReturnSuccess)return result;
    }

    /* panfrost_active_slots() (panfrost_job.c:637-656) under
     * readl_poll_timeout() (:704-706). The mask survives across polls: one
     * DONE/ERR event retires that slot for the rest of the wait, even if
     * JS_STATE has not caught up yet. */
    uint32_t activeMask = 0xFFFFFFFFu;
    uint32_t jsState = 0;
    uint64_t waited = 0;
    for(;;) {
        result = registers_->read32(JOB_INT_JS_STATE, &jsState);
        if(result != kIOReturnSuccess)return result;
        if(jsState & activeMask) {
            uint32_t rawstat = 0;
            result = registers_->read32(JOB_INT_RAWSTAT, &rawstat);
            if(result != kIOReturnSuccess)return result;
            for(uint32_t js = 0; js < NativeMaliJobSlotCount; ++js)
                if(rawstat & MK_JS_MASK(js))activeMask &= ~MK_JS_MASK(js);
        }
        /* Note: the source passes NUM_JOB_SLOTS to the clearing loop, so a
         * JS_STATE bit outside the three slots would keep the poll alive until
         * the timeout. Hardware only reports the slots it has, and this driver
         * has already checked GPU_JS_PRESENT, so the same rule is used. */
        if((jsState & activeMask) == 0)return kIOReturnSuccess;
        if(waited >= SoftStopTimeoutUs)return kIOReturnTimeout;
        result = registers_->delay(SoftStopPollUs);
        if(result != kIOReturnSuccess)return result;
        waited += SoftStopPollUs;
    }
}

void NativeMaliJobSlot::abandonOutstanding()
{
    /* No registers: the reset already destroyed the hardware state. The seqnos
     * stay, because a fence is only ever compared and the source does not
     * rewind emit_seqno. */
    for(uint32_t js = 0; js < NativeMaliJobSlotCount; ++js) {
        outstanding_[js] = false;
        faulted_[js] = false;
    }
}

IOReturn NativeMaliJobSlot::clearFault(uint32_t js)
{
    if(js >= NativeMaliJobSlotCount)return kIOReturnBadArgument;
    if(outstanding_[js])return kIOReturnBusy;
    faulted_[js] = false;
    return kIOReturnSuccess;
}
