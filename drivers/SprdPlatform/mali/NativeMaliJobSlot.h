/* SPDX-License-Identifier: GPL-2.0-only */
/* One job slot: everything a job chain needs before the GPU may start it, and
 * the polled wait for it to finish. No descriptor building (NativeMaliJob.h)
 * and no interrupts; this is panfrost_job_hw_submit() plus the DONE/ERR part of
 * panfrost_jm_handle_irq() without the scheduler behind it.
 *
 * Source (content-pinned, third_party/PINNED.md):
 *   third_party/linux-panfrost/drivers/gpu/drm/panfrost/panfrost_job.c:205-282
 *     panfrost_job_hw_submit(): refuse if JS_COMMAND_NEXT is non-zero, take the
 *     address space number from panfrost_mmu_as_get(), write
 *     JS_HEAD_NEXT_LO/HI = job->jc, the affinity, JS_CONFIG_NEXT, the flush id
 *     when the feature exists, and finally JS_COMMAND_NEXT = JS_COMMAND_START.
 *   panfrost_job.c:135-151
 *     panfrost_job_write_affinity(): JS_AFFINITY_NEXT_LO/HI =
 *     features.shader_present, "use all cores for now".
 *   panfrost_job.c:103-110, 152-161
 *     the chain flag is JS_CONFIG_JOB_CHAIN_FLAG when the per-slot
 *     emit_seqno (++emit_seqno, so the first job is 1) is odd and
 *     HW_FEATURE_JOBCHAIN_DISAMBIGUATION exists.
 *   panfrost_job.c:233-253
 *     cfg starts as the address space number ("AS %d" from cfg & 0xf) and gets
 *     JS_CONFIG_THREAD_PRI(8), START_FLUSH_CLEAN_INVALIDATE,
 *     END_FLUSH_CLEAN_INVALIDATE and, on a GPU with HW_FEATURE_FLUSH_REDUCTION,
 *     JS_CONFIG_ENABLE_FLUSH_REDUCTION plus a JS_FLUSH_ID_NEXT write.
 *     JS_CONFIG_START_MMU is guarded by HW_ISSUE_10649.
 *   panfrost_features.h:73-83
 *     hw_features_g52 has JOBCHAIN_DISAMBIGUATION and FLUSH_REDUCTION.
 *   panfrost_issues.h:248
 *     #define hw_issues_g52 0, so START_MMU is never set on this GPU, and
 *     panfrost_issues.h:141-142 (hw_issues_all = HW_ISSUE_9435) adds nothing
 *     here.
 *   panfrost_gpu.c:561-573
 *     panfrost_gpu_get_latest_flush_id() reads GPU_LATEST_FLUSH_ID while the
 *     GPU is in use; a flush id of 0 means "nothing was reduced".
 *   panfrost_job.c:544-550
 *     the interrupt handler cancels a staged submission with
 *     JS_COMMAND_NEXT = JS_COMMAND_NOP; that is the rollback used here.
 *   panfrost_job.h:48
 *     NUM_JOB_SLOTS 3: the driver uses at most three job slots, whatever
 *     GPU_JS_PRESENT reports.
 *   panfrost_job.c:520-545, 560-571
 *     panfrost_jm_handle_irq(): DONE is BIT(js) of JOB_INT_RAWSTAT and ERR is
 *     BIT(js + 16); on ERR the handler cancels the staged next command with
 *     JS_COMMAND_NEXT = JS_COMMAND_NOP, clears through JOB_INT_CLEAR the bits it
 *     read, and re-reads RAWSTAT until it is zero. That re-read exists because
 *     JOB_INT_CLEAR also samples JOB_INT_JS_STATE, the queue is two jobs deep
 *     per slot and only JS_STATE can tell the two apart. This driver keeps one
 *     job per slot, so DONE/ERR alone already name it and JS_STATE is not read
 *     to identify a completion; quiesce() reads it only for the soft-stop wait.
 *   panfrost_job.c:454-470
 *     panfrost_job_handle_err() reads JS_STATUS(js) for the reason. Its low 8
 *     bits are the same exception table as GPU_FAULT_STATUS, which
 *     NativeMaliFault decodes.
 *   panfrost_device.h:262-268, 326-331
 *     the exception codes: DONE 0x01, STOPPED 0x03 (resumable; the source
 *     restarts the slot from JS_TAIL), TERMINATED 0x04 (cancelled), and
 *     anything above MAX_NON_FAULT 0x3f is a fault.
 *   panfrost_job.c:435-438 and panfrost_device.h:29-31
 *     panfrost_jm_reset_interrupts() writes ALL_JS_INT_MASK to JOB_INT_CLEAR
 *     (GENMASK(16 + NUM_JOB_SLOTS - 1, 16) | GENMASK(NUM_JOB_SLOTS - 1, 0)),
 *     because a GPU reset can leave a stale job event latched. This driver's
 *     reset path (NativeMaliGpuCore::softReset) only knows GPU_INT_CLEAR, so
 *     submit() clears its own slot's bits before starting.
 *   panfrost_job.c:697-709
 *     panfrost_reset() quiesces the job manager before panfrost_device_reset():
 *     every slot gets JS_COMMAND_NEXT = JS_COMMAND_NOP and
 *     JS_COMMAND = JS_COMMAND_SOFT_STOP, then JOB_INT_JS_STATE is polled every
 *     10 us for at most 10 ms. A timeout only logs "Soft-stop failed"; the
 *     reset goes ahead anyway. quiesce() is that step, with the timeout kept as
 *     a distinct return so the caller can count it instead of losing it.
 *   panfrost_job.c:637-656
 *     panfrost_active_slots(): a slot is still active while its JS_STATE bit is
 *     set and no DONE/ERR bit for it has appeared in JOB_INT_RAWSTAT; the mask
 *     is kept across polls, so one event retires that slot for the rest of the
 *     wait.
 *
 * Completion is polled, not interrupt-driven: the JOB line from the DTB would
 * need an IOInterruptEventSource, a workloop and a command gate, and none of
 * that exists yet (docs/TODO.md, M3/M6). Polling JOB_INT_RAWSTAT is the part
 * that can be proven on the host and on the device first. JOB_INT_MASK is
 * therefore never unmasked and no interrupt is claimed.
 *
 * One job per slot at a time: submit() refuses while the previous job on that
 * slot has not been reaped. That is what makes DONE unambiguous without
 * JS_STATE, and it is a deliberate restriction, not a hardware limit (the
 * source queues two jobs per slot).
 *
 * The slot registers live at 0x1800 inside the same 0x13040000 window as the
 * GPU core, so they are only safe to touch after NativeMaliPower::gpuReadable();
 * submit() re-checks that gate on every call.
 *
 * Submit is one chain: the caller stops at JS_COMMAND_START and nothing here
 * waits for completion. A second submit while JS_COMMAND_NEXT is non-zero is a
 * kIOReturnBusy refusal, exactly the case the source treats as a WARN.
 *
 * Writes are behind NATIVE_MALI_M6_WRITES (default 0): starting a job before a
 * human has accepted the probe output would be the first step that lets the GPU
 * fetch from memory.
 */
#ifndef NATIVE_MALI_JOB_SLOT_H
#define NATIVE_MALI_JOB_SLOT_H
#include "NativeMaliMmu.h"
#include "NativeMaliPlatform.h"
#include "NativeMaliPower.h"
#include <IOKit/IOTypes.h>
#include <stdint.h>

#ifndef NATIVE_MALI_M6_WRITES
#define NATIVE_MALI_M6_WRITES 0
#endif

enum : uint32_t {
    /* panfrost_job.h:48. GPU_JS_PRESENT may report more; the source only ever
     * drives three. */
    NativeMaliJobSlotCount = 3,
    /* v7.xml:1334 "<struct name="Job Header" align="64">": a job chain head has
     * to be 64-byte aligned. */
    NativeMaliJobHeadAlignment = 64,
    /* The address space field is the low nibble of JS_CONFIG; the source's only
     * description of it is the "AS %d" debug print of cfg & 0xf
     * (panfrost_job.c:280). */
    NativeMaliJobMaxAddressSpace = 0xF,
    /* ALL_JS_INT_MASK (panfrost_device.h:29-31) with NUM_JOB_SLOTS = 3
     * (panfrost_job.h:48): GENMASK(18,16) | GENMASK(2,0). Written by
     * panfrost_jm_reset_interrupts() (panfrost_job.c:435-438). The low three
     * bits are DONE, the high three ERR (MK_JS_MASK, panfrost_regs.h:240). */
    NativeMaliAllJobInterrupts =
        ((1u << NativeMaliJobSlotCount) - 1u) * 0x10001u
};

enum : uint64_t {
    /* A WRITE_VALUE chain is microseconds of work; the budget only exists so a
     * wedged JM cannot hang the caller. Same 100 ms as the MMU's ready poll. */
    DefaultCompletionTimeoutUs = 100000,
    CompletionPollUs = 10,
    /* panfrost_job.c:703-706: "Wait at most 10ms for soft-stops to complete",
     * readl_poll_timeout(..., 10, 10000) - 10 us between reads, 10 ms total. */
    SoftStopTimeoutUs = 10000,
    SoftStopPollUs = 10
};

/* A software fence: the number the chain was started with on one job slot, the
 * same value panfrost puts in fence->seqno (panfrost_job.c:106,
 * fence->seqno = ++queue[js].emit_seqno, so a slot's first job is 1). There is
 * no dma_fence here, so "signaled" only means waitForCompletion() has reaped
 * that job; the reason it ended is in the NativeMaliJobCompletion the reaper
 * received, not in the fence. */
struct NativeMaliFence {
    uint32_t js = 0;
    uint32_t seqno = 0;

    bool valid() const { return seqno != 0; }
};

/* What ended a wait. `rawStatus` is the JOB_INT_RAWSTAT word that had this
 * slot's bit set; `jsStatus` is JS_STATUS(js), which is only read for an error
 * and whose low 8 bits NativeMaliFault::exceptionName() decodes. Both stay 0
 * when no event was seen (timeout or a gate/read error). On the error path they
 * are filled before the teardown writes, so they are still the truth if one of
 * those writes then fails. */
struct NativeMaliJobCompletion {
    uint32_t rawStatus = 0;
    uint32_t jsStatus = 0;

    uint32_t exception() const { return jsStatus & 0xFF; }
    /* panfrost_exception_is_fault(): everything above MAX_NON_FAULT (0x3f) is
     * a fault, which is what NativeMaliFault::isFault() already encodes. */
    bool isFault() const { return exception() > NativeMaliFaultMaxNonFault; }
};

class NativeMaliJobSlot final {
public:
    /* False unless the build opts in; the driver build never sets it. */
    static constexpr bool kWriteAuthorized = NATIVE_MALI_M6_WRITES != 0;

    NativeMaliJobSlot() = default;
    ~NativeMaliJobSlot() = default;
    NativeMaliJobSlot(const NativeMaliJobSlot &) = delete;
    NativeMaliJobSlot &operator=(const NativeMaliJobSlot &) = delete;

    /* `registers` is the whole 0x13040000 page (the slots start at 0x1800),
     * `power` must already be prepared, and `mmu` is what says whether an
     * address space may be named by a job. Re-preparing with the same triple is
     * allowed; a different one is refused. */
    IOReturn prepare(NativeMaliRegisterTarget *registers, NativeMaliPower *power,
        NativeMaliMmu *mmu);
    bool prepared() const
    {
        return registers_ && power_ && mmu_ && power_->prepared() &&
            mmu_->prepared();
    }

    /* Record GPU_JS_PRESENT and GPU_SHADER_PRESENT from NativeMaliGpuCore's
     * probe. Both must be non-zero; a different pair after the first call is
     * refused, because the slots and cores already used would no longer mean
     * the same hardware. */
    IOReturn setPresent(uint32_t jsPresent, uint64_t shaderPresent);
    uint32_t present() const { return jsPresent_; }
    uint64_t shaderPresent() const { return shaderPresent_; }

    /* The exact JS_CONFIG_NEXT word panfrost_job_hw_submit() builds for this
     * GPU: the address space in the low nibble, thread priority 8, clean and
     * invalidate on both ends of the job, flush reduction (G52 has it) and,
     * when `jobChain` is set, the disambiguation flag. Pure. */
    static uint32_t configValue(uint32_t as, bool jobChain);

    /* panfrost_job_hw_submit(): start the chain whose first descriptor is at
     * `head` on slot `js`.
     *
     * Refusals, all before the first write: not prepared, M6 writes compiled
     * out, GPU not readable, `js` outside NativeMaliJobSlotCount or not in
     * GPU_JS_PRESENT, `as` outside NativeMaliJobMaxAddressSpace or not
     * allocated in the MMU, or a `head` of 0 or not 64-byte aligned.
     * kIOReturnBusy when JS_COMMAND_NEXT is not zero, i.e. another command is
     * staged (the source's WARN_ON).
     *
     * `flushId` is GPU_LATEST_FLUSH_ID (NativeMaliGpuCore::latestFlushId()); it
     * is written unconditionally because the flush-reduction feature is always
     * on for this GPU.
     *
     * A write failure cancels the staged job with JS_COMMAND_NEXT =
     * JS_COMMAND_NOP (best effort) and returns the original error. The slot's
     * chain parity is only advanced by a successful start, so a retry is the
     * same chain number.
     *
     * kIOReturnBusy also when a job submitted earlier on this slot has not been
     * reaped by waitForCompletion() yet, and kIOReturnNotReady when the slot is
     * faulted (see clearFault()).
     *
     * On success `fence`, when given, receives the slot and the seqno the chain
     * was started with (panfrost_job.c:106). It is written only on success and
     * zeroed on entry, so a refusal cannot leave a believable fence behind. */
    IOReturn submit(uint32_t js, uint64_t head, uint32_t as, uint32_t flushId,
        NativeMaliFence *fence = nullptr);

    /* Wait for the job submitted on `js` to finish, polling JOB_INT_RAWSTAT.
     *
     *   kIOReturnSuccess    DONE arrived; the slot's DONE bit is cleared and the
     *                       slot is free for the next submit.
     *   kIOReturnIOError    ERR arrived. `completion->jsStatus` holds the
     *                       reason; the staged next command is cancelled with
     *                       JS_COMMAND_NEXT = JS_COMMAND_NOP, the slot's bits
     *                       are cleared, and the slot is latched as faulted
     *                       (submit() refuses until clearFault()).
     *   kIOReturnTimeout    neither bit arrived in time. Nothing is written and
     *                       the job stays outstanding, so the call can be
     *                       repeated; a timeout is not a completion.
     *   other               gate/read/write error, returned unchanged.
     *
     * Only this slot's DONE and ERR bits are cleared: the source clears the
     * whole word, but it also samples JS_STATE under that write, and clearing
     * another slot's event here would steal that slot's completion. */
    IOReturn waitForCompletion(uint32_t js,
        NativeMaliJobCompletion *completion,
        uint64_t timeout_us = DefaultCompletionTimeoutUs,
        uint64_t poll_us = CompletionPollUs);

    /* panfrost_jm_reset_interrupts() (panfrost_job.c:435-438): one write of
     * JOB_INT_CLEAR = ALL_JS_INT_MASK. A GPU reset can latch a job event that
     * no job of ours produced, so the whole palette is cleared before anything
     * is submitted again. This only touches the event word; it says nothing
     * about the jobs themselves - panfrost stops its schedulers separately,
     * and this driver's equivalent is abandonOutstanding().
     *
     * Requires a prepared slot, kWriteAuthorized and a known GPU_JS_PRESENT
     * (setPresent(), as submit() already does: the source's call site is after
     * panfrost_jm_init()), and goes through gpuReadable() like every other
     * write. A write failure is returned unchanged. No output is zeroed because
     * there is none; calling it twice writes the same word twice, which is
     * harmless and matches the source. */
    IOReturn resetInterrupts();

    /* The job-manager quiesce panfrost_reset() runs before the hardware reset
     * (panfrost_job.c:697-709): for every slot in GPU_JS_PRESENT, cancel the
     * staged next command with JS_COMMAND_NEXT = JS_COMMAND_NOP and ask the
     * running chain to stop with JS_COMMAND = JS_COMMAND_SOFT_STOP; then poll
     * JOB_INT_JS_STATE every 10 us for at most 10 ms until no slot is active.
     *
     * A slot is active while its JS_STATE bit is set and no DONE/ERR bit for it
     * has appeared in JOB_INT_RAWSTAT; once an event is seen, that slot is
     * masked out of the remaining polls (panfrost_active_slots(),
     * panfrost_job.c:637-656). Only the slots GPU_JS_PRESENT names are written:
     * the source loops over all three because it drives all three, but a slot
     * this GPU does not have is not one this driver may touch, and submit()
     * already refuses it for the same reason.
     *
     * kIOReturnSuccess  every slot went quiet.
     * kIOReturnTimeout  some slot was still active after 10 ms. The source logs
     *                   "Soft-stop failed" and resets anyway (:708-709). This is
     *                   a distinct answer, not a swallowed success, so the
     *                   caller decides and can record it.
     * other             a gate or register error, returned unchanged.
     *
     * Requires a prepared slot, kWriteAuthorized, a known GPU_JS_PRESENT and a
     * readable GPU, the same gates as resetInterrupts(). */
    IOReturn quiesce();

    /* The scheduler half of panfrost_reset(): the reset worker stops every
     * queue and signals the in-flight fences with an error before the hardware
     * is reset (drm_sched_stop() in panfrost_reset(), and
     * panfrost_device_reset() itself). This driver has no scheduler and no job
     * object, so the equivalent is a pure state drop: the reset destroyed every
     * in-flight job, so no slot is outstanding and no slot is faulted any more.
     * `emitSeqno_`/`completedSeqno_` are deliberately kept - the source does
     * not reset emit_seqno either, and they are only compared.
     *
     * Pure: no register, no gate, idempotent. Call it after the hardware reset
     * (softReset -> powerOn -> resetAllocation -> resetInterrupts); the clients
     * that held those fences learn about the loss from
     * NativeMaliUserClient::noteReset(), which reads latestSeqno() >
     * completedSeqno() and therefore does not depend on the order of the two
     * calls. A fence that was in flight here is never signaled: it was not
     * reaped by a completion, which is what fenceSignaled() means. */
    void abandonOutstanding();

    /* True between a successful submit() and a waitForCompletion() that reaped
     * it. A false answer for a slot that does not exist is also false. */
    bool outstanding(uint32_t js) const
    {
        return js < NativeMaliJobSlotCount && outstanding_[js];
    }
    /* True after waitForCompletion() saw ERR on `js`; submit() refuses such a
     * slot until clearFault(). False for a slot that does not exist. */
    bool faulted(uint32_t js) const
    {
        return js < NativeMaliJobSlotCount && faulted_[js];
    }

    /* Fence bookkeeping. The seqno is the one submit() gave the chain above, so
     * a client can keep "(slot, seqno)" and ask later whether that job is done
     * without waiting:
     *
     *   latestSeqno(js)     the last seqno submit() started on `js` (0 before
     *                       the first job).
     *   completedSeqno(js)  the highest seqno waitForCompletion() has reaped
     *                       there. Jobs on one slot complete in order, so
     *                       `fence.seqno <= completedSeqno(fence.js)` means the
     *                       job is no longer in flight - it says nothing about
     *                       success: a faulted job also signals, and the reason
     *                       is in the completion the reaper received.
     *   fenceSignaled()     the same test plus the slot range check, false for
     *                       an invalid or out-of-range fence.
     *
     * No "is this seqno even from the future" check is needed:
     * completedSeqno(js) is only ever assigned emitSeqno(js), so it can never
     * run ahead of what was submitted, and a seqno above latestSeqno(js) fails
     * the completed comparison on its own.
     *
     * panfrost signals its dma_fence from the interrupt handler; here the
     * signal is the successful reap, because completion is polled. Nothing is
     * reset by clearFault() or by a GPU reset: the seqno is only ever
     * compared, and panfrost does not reset emit_seqno either. */
    uint32_t latestSeqno(uint32_t js) const
    {
        return js < NativeMaliJobSlotCount ? emitSeqno_[js] : 0;
    }
    uint32_t completedSeqno(uint32_t js) const
    {
        return js < NativeMaliJobSlotCount ? completedSeqno_[js] : 0;
    }
    bool fenceSignaled(const NativeMaliFence &fence) const
    {
        if(!fence.valid() || fence.js >= NativeMaliJobSlotCount)return false;
        return fence.seqno <= completedSeqno_[fence.js];
    }
    /* Forget the fault latch so the slot may be used again. This touches no
     * hardware: the caller is expected to have reset the GPU (the source
     * schedules a reset whenever the exception needs one,
     * panfrost_device.h:333-338) and to have re-armed the AS/VM. Idempotent;
     * kIOReturnBusy while a job is still outstanding. */
    IOReturn clearFault(uint32_t js);

private:
    IOReturn writeSequence(uint32_t js, uint64_t head, uint64_t affinity,
        uint32_t config, uint32_t flushId);
    IOReturn writePair(uint32_t low, uint32_t high, uint64_t value);

    NativeMaliRegisterTarget *registers_ = nullptr;
    NativeMaliPower *power_ = nullptr;
    NativeMaliMmu *mmu_ = nullptr;
    uint32_t jsPresent_ = 0;
    uint64_t shaderPresent_ = 0;
    /* panfrost_job.c:106: fence->seqno = ++queue[js].emit_seqno, and the chain
     * flag is its parity, so a slot's first submission sets the flag. */
    uint32_t emitSeqno_[NativeMaliJobSlotCount]{};
    /* One job per slot, so the DONE bit belongs to the job this driver started
     * and waiting twice cannot consume someone else's event. */
    bool outstanding_[NativeMaliJobSlotCount]{};
    bool faulted_[NativeMaliJobSlotCount]{};
    /* The highest fence seqno reaped on each slot (emitSeqno_ is the last one
     * started). Both start at 0, i.e. "nothing started/completed yet". */
    uint32_t completedSeqno_[NativeMaliJobSlotCount]{};
};
#endif
