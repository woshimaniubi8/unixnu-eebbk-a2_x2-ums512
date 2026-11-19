/* SPDX-License-Identifier: GPL-2.0-only */
/* One client of the GPU: its buffer objects, its one address space, and the
 * jobs it started. This is what the future NativeMaliService IOUserClient will
 * dispatch into; it is pure (no IOKit object, no register of its own), so the
 * host test can run the real page tables and the real job-slot handshake
 * against fake registers.
 *
 * Shape (NativeAdrenoUserClient.h, MIT, see NOTICE; nothing copied): the client
 * owns its objects, hands out its own fences, keeps the counters GetStatus
 * reports, and is usable only while open.
 *
 * Submission (panfrost_job.c):
 *   - The board inspects bounded, mapped job headers and routes FS to JS0,
 *     setup/geometry to JS1 and compute to JS2. Mixed-queue chains and cycles
 *     are rejected before a command is written.
 *   - The chain head must be inside one of this client's live mappings; a GPU
 *     address the client never mapped is refused before any register is
 *     written. panfrost gets the same guarantee from the GEM mapping it looks
 *     the job up in.
 *   - The flush id is read per submission from GPU_LATEST_FLUSH_ID
 *     (panfrost_gpu_get_latest_flush_id(), panfrost_gpu.c:561-573), exactly
 *     where panfrost_job_hw_submit() reads it; it is never the client's to
 *     choose.
 *   - The fence is the slot's emit seqno (panfrost_job.c:106), packed as the
 *     ABI documents. A dependency fence and WaitFence must be one this client
 *     was handed. Only the newest in-flight fence of this client can reap a
 *     slot. Older, completed seqnos are queries; they never access the
 *     client's GPU buffers while a newer job might be using them.
 *   - Completion is polled synchronously (NativeMaliJobSlot), so a wait here
 *     blocks. The interrupt-driven form is still blocked on
 *     docs/OPEN_QUESTIONS.md item 14.
 *
 * Counters: `faults` counts reaped completions whose exception is a fault
 * (NativeMaliJobCompletion::isFault(), the source's reset-worthy set); the
 * service calls noteReset() when it resets the GPU. A changed faults or resets
 * is how userspace learns that older fences are dead (NativeMaliABI.h).
 * noteReset() additionally remembers the one seqno per slot that was in flight
 * at that moment, so waiting on exactly that fence returns IOError (an error
 * fence, as panfrost signals) rather than timing out on a job the reset
 * destroyed. Unlike a reaped ERR it does not advance `faults`: the GPU was
 * reset, not the job faulted.
 *
 * Not here: allocating the address space (the service does that), the MMU
 * enable/disable around it, GPU resets, and the IOUserClient object itself.
 */
#ifndef NATIVE_MALI_USER_CLIENT_H
#define NATIVE_MALI_USER_CLIENT_H
#include "NativeMaliABI.h"
#include "NativeMaliBoTable.h"
#include "NativeMaliGpuCore.h"
#include "NativeMaliJobSlot.h"
#include <IOKit/IOTypes.h>
#include <stdint.h>

/* The device-wide half of the wire ABI, injected so the per-client dispatch
 * stays free of the service. NativeMaliService implements this as reset(): a
 * reset destroys jobs of every client and reprograms every address space, so it
 * cannot belong to one client object. A client opened without a target answers
 * the Reset method with kIOReturnUnsupported instead of pretending it reset
 * anything. */
class NativeMaliResetTarget {
public:
    NativeMaliResetTarget() = default;
    virtual ~NativeMaliResetTarget() = default;
    NativeMaliResetTarget(const NativeMaliResetTarget &) = delete;
    NativeMaliResetTarget &operator=(const NativeMaliResetTarget &) = delete;

    virtual IOReturn resetDevice() = 0;
};

class NativeMaliUserClient final {
    NativeMaliBoTable bo_;
    NativeMaliJobSlot *jobs_ = nullptr;
    NativeMaliGpuCore *gpu_ = nullptr;
    NativeMaliVm *vm_ = nullptr;
    NativeMaliResetTarget *reset_ = nullptr;
    NativeMaliGpuObservation observation_{};
    uint32_t as_ = 0;
    uint64_t maxBoBytes_ = 0;
    /* The highest seqno this client received on each slot. 0 means it never
     * submitted there, which is also why fence seqno 0 is invalid. */
    uint32_t submitted_[NativeMaliJobSlotCount]{};
    /* Last failed or reset-killed fence on each slot. The same fence stays
     * failed on a repeated wait, even after the hardware has been reaped. */
    uint32_t deadSeqno_[NativeMaliJobSlotCount]{};
    // One job per client. A reaped job still owns the buffers until its CPU
    // sync succeeds, including when the cache maintainer reports an error.
    NativeMaliFence pendingSync_{};
    uint32_t nextSlot_ = 0;
    uint64_t faults_ = 0;
    uint64_t resets_ = 0;
    bool open_ = false;

    IOReturn chooseSlot(uint64_t chain, uint32_t *slot);
    bool hasOutstanding() const;
    IOReturn finishCpuSync();
    bool ownOutstanding(uint32_t js) const;
    IOReturn waitForFence(const NativeMaliFence &fence, uint64_t timeout_ns,
        NativeMaliJobCompletion *completion);
    static uint64_t timeoutMicroseconds(uint64_t timeout_ns);
public:
    NativeMaliUserClient() = default;
    ~NativeMaliUserClient() = default;
    NativeMaliUserClient(const NativeMaliUserClient &) = delete;
    NativeMaliUserClient &operator=(const NativeMaliUserClient &) = delete;

    /* Bind this client's address space and the shared job/GPU blocks. `as` is
     * the slot the service allocated for this client and passed to
     * NativeMaliMmu::enable(); `observation` is the M3 probe output the
     * Query/GetDeviceInfo replies are built from. A non-coherent client needs
     * a cache maintainer, refused once by NativeMaliBoTable::open(). `reset` is
     * the service (or a fake in a test); it is what the Reset selector calls,
     * and may be null when the caller does not offer that method. */
    IOReturn open(NativeMaliMemorySource *source,
        NativeMaliCacheMaintainer *maintainer, bool cpuCoherent,
        NativeMaliVm *vm, NativeMaliMmu *mmu, uint32_t as,
        NativeMaliJobSlot *jobs, NativeMaliGpuCore *gpu,
        const NativeMaliGpuObservation &observation, uint64_t maxBoBytes,
        NativeMaliResetTarget *reset = nullptr);
    bool open() const { return open_; }

    /* Objects: the NativeMaliBoTable entry points, so the service dispatches
     * every method on this one object. Each validates and zeroes its reply
     * exactly as the table documents. */
    IOReturn allocBo(const NativeMaliAbiBoAlloc &request,
        NativeMaliAbiBoAllocReply *reply);
    IOReturn mapBo(const NativeMaliAbiBoMap &request,
        NativeMaliAbiBoMapReply *reply);
    IOReturn freeBo(const NativeMaliAbiBoFree &request);
    IOReturn cpuMap(const NativeMaliAbiBoCpuMap &request,
        NativeMaliAbiBoCpuMapReply *reply);
    IOReturn cpuUnmap(const NativeMaliAbiBoCpuUnmap &request);
    NativeMaliMemory *memoryForToken(uint32_t token)
    {
        return bo_.memoryForToken(token);
    }

    /* Read-only facts. input/output bits are the geometry of the tables the
     * service actually built (NativeMaliVm::info()), not what the hardware
     * could support, so a mapping is never advertised beyond what exists. */
    IOReturn query(const NativeMaliAbiQuery &request,
        NativeMaliAbiQuery *reply);
    IOReturn deviceInfo(NativeMaliAbiDeviceInfo *reply);

    /* Start a job chain, waiting for `wait_fence` first when it is nonzero.
     * Refusals before any write: not open, malformed record, a chain head
     * outside this client's mappings, an unknown/higher-than-submitted
     * dependency fence, a dependency that times out or faults, no idle slot
     * (Busy), or the flush-id read failing. The reply fence is zeroed on every
     * refusal. */
    IOReturn submit(const NativeMaliAbiSubmit &request,
        NativeMaliAbiSubmitReply *reply);
    /* Wait until the job that produced this fence has been reaped. Success is
     * "no longer in flight"; the wait that actually reaps an ERR returns
     * IOError with the faults counter advanced. A repeat wait on a fence that
     * is already reaped returns Success, because the reason was in the
     * completion the first wait received and the counter is what survives.
     * Timeout leaves it outstanding and the call can be repeated. A fence this
     * client was not handed is refused without touching a slot. */
    IOReturn waitFence(const NativeMaliAbiWaitFence &request);
    IOReturn status(NativeMaliAbiStatus *reply);

    /* The external-method entry point: the size/record half of the future
     * IOUserClient subclass. Every NativeMaliMethod* names exactly one input
     * record and one output record (either may be absent), so a caller cannot
     * pass a length that disagrees with the wire format.
     *
     *   kIOReturnUnsupported   unknown method (that is a newer client, not a
     *                          malformed request).
     *   kIOReturnBadArgument   wrong input or output size, or a null buffer
     *                          where the record is required.
     *   anything else          exactly what the typed method above returned.
     *
     * No output byte is written unless the typed method succeeded, so a refusal
     * cannot leave a believable reply behind (the same rule the records use).
     * Reset is the one method that is not about this client alone: it is
     * forwarded to the NativeMaliResetTarget injected at open(), and answers
     * kIOReturnUnsupported when there is none.
     *
     * This does not decide whether the GPU is usable: the service gate
     * (`NativeMaliService::clientUsable()`) belongs above it, where the service
     * is known; here the client's own open/NotReady checks still apply.
     *
     * The dispatch shape - exact sizes, unknown method unsupported, untouched
     * output on failure - follows NativeAdrenoUserClient::dispatch() (MIT, see
     * NOTICE); none of its records or code are copied. */
    IOReturn dispatch(uint32_t method, const void *input, size_t input_size,
        void *output, size_t output_size);
    /* Exact record sizes for a method, or kUnsupportedSize. Pure. */
    static size_t inputSize(uint32_t method);
    static size_t outputSize(uint32_t method);
    static constexpr size_t kUnsupportedSize = static_cast<size_t>(-1);

    /* The service calls this around a GPU reset, so GetStatus reports the
     * context loss and a wait on a fence that was in flight answers IOError
     * instead of hanging. A slot whose latestSeqno() is still above its
     * completedSeqno() had a job that the reset destroyed, and that seqno is
     * remembered here; the order relative to NativeMaliJobSlot::abandonOutstanding()
     * does not matter, because abandonOutstanding() keeps both seqnos.
     * A later reset that kills nothing on a slot leaves an older marker in
     * place: completedSeqno() only moves forward, so clearing it would make the
     * destroyed fence look like an ordinary reaped one.
     * It does not touch the job slots: their allocation and fault state belong
     * to the bring-up path. Not counted when the client is closed. */
    void noteReset();

    /* Refuse while any object or any of this client's jobs is live; otherwise
     * close the object table. Nothing is forced. */
    IOReturn close();
    /* Connection teardown: wait (up to `timeout_us` per job) for every job this
     * client started, then unmap and free every object. The first failure
     * stops it and leaves the rest live, so the caller can retry. */
    IOReturn releaseAll(uint64_t timeout_us = DefaultCompletionTimeoutUs);
    /* Teardown when the block is gone: the service has checked that the GPU is
     * not readable, so there is no leaf to clear, no flush to wait for and no
     * job that can still finish. Drop every object through the no-register
     * path NativeMaliBoTable::reset() documents (the source gets its pages
     * back) and close. The caller still owns the address-space number and the
     * page tables; this touches neither. */
    IOReturn abandon();

    size_t liveObjects() const { return bo_.live(); }
    NativeMaliMemory *memoryForHandle(uint64_t handle) { return bo_.memory(handle); }
};

#endif
