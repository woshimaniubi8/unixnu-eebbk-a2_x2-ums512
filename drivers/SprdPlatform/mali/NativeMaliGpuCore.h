/* SPDX-License-Identifier: GPL-2.0-only */
/* GPU register access: what the block reports about itself, and its reset.
 *
 * Source (content-pinned, third_party/PINNED.md):
 *   drivers/gpu/drm/panfrost/panfrost_gpu.c:62-108
 *     panfrost_gpu_soft_reset(): INT_MASK = 0, clear RESET_COMPLETED, issue
 *     GPU_CMD_SOFT_RESET, poll RAWSTAT every 10 us for 10 ms; on timeout issue
 *     GPU_CMD_HARD_RESET and poll every 100 us; then INT_CLEAR = MASK_ALL and
 *     INT_MASK = MASK_ERROR | PERFCNT_SAMPLE_COMPLETED |
 *     CLEAN_CACHES_COMPLETED.
 *   drivers/gpu/drm/panfrost/panfrost_gpu.c:251-313
 *     panfrost_gpu_init_features() reads GPU_ID, the seven feature words,
 *     the thread limits, AS/JS present, JS_FEATURES(i) for
 *     i < hweight32(js_present), the 64-bit present bitmaps (LO | HI << 32)
 *     and THREAD_TLS_ALLOC.
 *   drivers/gpu/drm/panfrost/panfrost_gpu.c:309-312
 *     nr_core_groups = hweight64(l2_present).
 *   drivers/gpu/drm/panfrost/panfrost_device.h:61-92
 *     the field widths (js_features[16], 64-bit present bitmaps).
 *   drivers/gpu/drm/panfrost/panfrost_gpu.c:117-180
 *     panfrost_gpu_init_quirks(): the SC_* shader-config bits, the tiler clock
 *     gate override and the JM_CONFIG IDVS group size.
 *   drivers/gpu/drm/panfrost/panfrost_features.h:73-83 and
 *     panfrost_issues.h:141-142,248
 *     the G52 feature and issue bits the quirk branches test.
 *   drivers/gpu/drm/panfrost/panfrost_gpu.c:27-43
 *     panfrost_gpu_irq_handler(): GPU_FAULT_STATUS plus the
 *     GPU_FAULT_ADDRESS_HI/LO pair, decoded by NativeMaliFault::exceptionName().
 *   drivers/gpu/drm/panfrost/panfrost_regs.h
 *     the register offsets, generated into NativeMaliRegs.h.
 *
 * AFBC and the four texture feature words are read here because a Vulkan
 * backend needs them (pan_kmod_dev_props: afbc_features, texture_features[4]).
 * The coherency register is still not read: it only exists on models with
 * HW_FEATURE_COHERENCY_REG, and this device is non-coherent by DTB.
 *
 * The pan_kmod-shaped derivations below (maxTasksPerCore, registersPerCore,
 * tlsInstancesPerCore) mirror Mesa panfrost_kmod.c:138-175 at the pinned
 * revision. They are computed here so the values cross the ABI once, instead
 * of being re-derived (or guessed) in userspace.
 *
 * Safety: 0x13040000 may only be read while the MFG domains, the topckgen mux
 * and the mfgcfg gate are up (M2). probe() asks NativeMaliPower::gpuReadable()
 * first and returns its error without touching the GPU target. That is the M1
 * rule "read SPM before the GPU" encoded as a precondition rather than a
 * comment.
 *
 * No register is written by probe() or readFault(). softReset() is the first
 * write: it sits behind NATIVE_MALI_M3_WRITES (default 0) and behind the same
 * M2 gate.
 */
#ifndef NATIVE_MALI_GPU_CORE_H
#define NATIVE_MALI_GPU_CORE_H
#include "NativeMaliFault.h"
#include "NativeMaliGpu.h"
#include "NativeMaliPlatform.h"
#include "NativeMaliPower.h"
#include <IOKit/IOTypes.h>
#include <stdint.h>

#ifndef NATIVE_MALI_M3_WRITES
#define NATIVE_MALI_M3_WRITES 0
#endif

struct NativeMaliGpuObservation {
    NativeMaliGpuIdentity identity;
    uint32_t gpuId = 0;

    uint32_t l2Features = 0;
    uint32_t coreFeatures = 0;
    uint32_t tilerFeatures = 0;
    uint32_t memFeatures = 0;
    uint32_t mmuFeatures = 0;
    uint32_t afbcFeatures = 0;
    uint32_t textureFeatures[4]{};
    uint32_t threadFeatures = 0;
    uint32_t maxThreads = 0;
    uint32_t maxWorkgroupSize = 0;
    uint32_t maxBarrierSize = 0;
    uint32_t asPresent = 0;
    uint32_t jsPresent = 0;
    uint32_t jsFeatures[16]{};
    uint64_t shaderPresent = 0;
    uint64_t tilerPresent = 0;
    uint64_t l2Present = 0;
    uint64_t stackPresent = 0;
    uint32_t threadTlsAlloc = 0;

    /* Derived for pan_kmod_dev_props, from the registers above:
     *   maxTasksPerCore      = max(thread_features >> 24, 1)
     *   registersPerCore     = thread_features & 0xffff, arch fallback
     *   tlsInstancesPerCore  = thread_tls_alloc, or max_threads when zero
     * (Mesa panfrost_kmod.c:138-175, pinned 26.2.6.) */
    uint32_t maxTasksPerCore = 0;
    uint32_t registersPerCore = 0;
    uint32_t tlsInstancesPerCore = 0;

    /* Derived exactly as the source derives them: popcount of the masks. */
    uint32_t jsCount = 0;
    uint32_t coreCount = 0;
    uint32_t tilerCount = 0;
    uint32_t l2Count = 0;
    uint32_t coreGroups = 0;
    uint32_t stackCount = 0;

    /* GPU_MMU_FEATURES [7:0] and [15:8]. The MMU layer validates them; a
     * zero here means the block did not answer, not a 0-bit address space. */
    uint32_t ias = 0;
    uint32_t oas = 0;
};

class NativeMaliGpuCore final {
public:
    enum : uint64_t {
        DefaultResetTimeoutUs = 10000,
        SoftResetPollUs = 10,
        HardResetPollUs = 100
    };

    /* False unless the build opts in; the driver build never sets it. */
    static constexpr bool kWriteAuthorized = NATIVE_MALI_M3_WRITES != 0;

    NativeMaliGpuCore() = default;
    ~NativeMaliGpuCore() = default;
    NativeMaliGpuCore(const NativeMaliGpuCore &) = delete;
    NativeMaliGpuCore &operator=(const NativeMaliGpuCore &) = delete;

    /* `gpu` is the 0x13040000 page. `power` must already be prepared; probe()
     * re-checks the live state on every call. */
    IOReturn prepare(NativeMaliRegisterTarget *gpu, NativeMaliPower *power);
    bool prepared() const { return gpu_ && power_ && power_->prepared(); }

    /* Fills `observation` and returns:
     *   kIOReturnSuccess     the identity matched a pinned model
     *   kIOReturnUnsupported the id is not in the pinned table (the fields are
     *                        still filled so the caller can log what it saw)
     *   other                the gate or a register read failed; the fields
     *                        read before the failure are filled
     * Refuses kIOReturnNotReady without reading the GPU if the M2 gate says
     * the block may not be touched. */
    IOReturn probe(NativeMaliGpuObservation *observation);

    /* panfrost_gpu_soft_reset(): mask interrupts, clear the reset-completed
     * bit, issue GPU_CMD_SOFT_RESET and poll GPU_INT_RAWSTAT for it; on timeout
     * issue GPU_CMD_HARD_RESET and poll again; then clear everything and mask
     * only the three sources panfrost keeps. A hard reset that also times out
     * returns kIOReturnTimeout and leaves the mask cleared, so the caller must
     * not continue.
     * Refused with kIOReturnUnsupported unless kWriteAuthorized, and with
     * kIOReturnNotReady unless the M2 gate says the block may be touched. */
    IOReturn softReset(uint64_t timeout_us = DefaultResetTimeoutUs);

    /* panfrost_gpu_irq_handler() (panfrost_gpu.c:27-43): GPU_FAULT_STATUS and
     * the GPU_FAULT_ADDRESS_HI/LO pair, decoded by NativeMaliFault. Read-only,
     * so no write switch, but it still refuses unless the M2 gate says the
     * block may be read. The fields read before a failure are kept, like
     * probe(). */
    IOReturn readFault(NativeMaliGpuFault *fault);

    /* panfrost_gpu_get_latest_flush_id() (panfrost_gpu.c:561-573): the
     * GPU_LATEST_FLUSH_ID word. The source returns 0 when the GPU is not in use
     * (pm_runtime_get_if_in_use); here a gate failure returns its error instead,
     * so a caller about to name a flush id is never handed a value that claims
     * nothing needs flushing. Read-only, but it still refuses unless the M2 gate
     * says the block may be read. G52 has HW_FEATURE_FLUSH_REDUCTION
     * (panfrost_features.h:73-83), so a job's JS_FLUSH_ID_NEXT comes from
     * here. */
    IOReturn latestFlushId(uint32_t *flushId);

    /* panfrost_gpu_power_on() (panfrost_gpu.c:455-484): L2, then shader, then
     * tiler, each PWRON followed by polling its READY bitmap for an exact
     * match. Uses the masks from the last successful probe() and only the
     * first core group, as panfrost_get_core_mask() (panfrost_gpu.c:432-453)
     * does. Requires a successful probe() and softReset(); on failure it
     * quiesces with powerOff() and returns the original error. Refuses masks
     * with bits above 31: only the LO registers are written here, and silently
     * dropping a high core would report ready for a partial GPU.
     *
     * The source runs panfrost_gpu_init_quirks() before L2; initQuirks() does
     * the same for the only model this driver accepts. */
    IOReturn powerOn();

    /* panfrost_gpu_power_off() (panfrost_gpu.c:486-504): shader, then tiler,
     * then L2, each PWROFF followed by polling PWRTRANS to zero. Every step is
     * attempted even if an earlier one fails, so a timed-out transition still
     * leaves the other blocks quiescing; the first error is returned. Clears
     * the reset-done state, because the next power-up starts with a reset.
     * Requires a successful probe() but deliberately not a reset, so a suspend
     * path can quiesce blocks that were never brought up. */
    IOReturn powerOff();

private:
    /* panfrost_gpu_init_quirks() (panfrost_gpu.c:117-180) for the one model
     * this driver accepts. G52 r1p0 has HW_FEATURE_IDVS_GROUP_SIZE
     * (panfrost_features.h:73-83) and no issue this function tests
     * (hw_issues_g52 is empty, hw_issues_all is only HW_ISSUE_9435,
     * panfrost_issues.h:141-142,248), so the only register value it needs is
     * the IDVS group size in GPU_JM_CONFIG. The source also reads
     * GPU_TILER_CONFIG and writes it back to set TC_CLOCK_GATE_OVERRIDE for
     * HW_ISSUE_T76X_3953; G52 does not have that issue, so that write would
     * write back the value just read and is skipped. The caller has already
     * checked the gate, the reset and kWriteAuthorized. */
    IOReturn initQuirks();
    IOReturn powerMasks(uint32_t *l2, uint32_t *shader,
        uint32_t *tiler) const;
    IOReturn powerWait(uint32_t offset, uint32_t expected, uint32_t poll_us,
        uint64_t timeout_us);
    IOReturn powerOffSteps();

    NativeMaliRegisterTarget *gpu_ = nullptr;
    NativeMaliPower *power_ = nullptr;
    NativeMaliGpuObservation observation_{};
    bool observationValid_ = false;
    bool resetDone_ = false;
};
#endif
