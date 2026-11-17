/* SPDX-License-Identifier: GPL-2.0-only */
/* GPU feature reads and the reset handshake; offsets and sources are in
 * NativeMaliGpuCore.h. */
#include "NativeMaliGpuCore.h"
#include "NativeMaliRegs.h"

extern "C" void sprd_mali_trace_mark(unsigned line, const char *text);

static void resetMark(unsigned line, const char *text)
{
    sprd_mali_trace_mark(line, text);
}

namespace {

/* panfrost_gpu_power_on/off() poll these; the values are the source's own
 * delays and budgets (panfrost_gpu.c:455-504). */
enum : uint32_t {
    L2PowerPollUs = 10,
    ShaderPowerPollUs = 10,
    TilerPowerPollUs = 10,
    L2PowerTimeoutUs = 20000,
    ShaderPowerTimeoutUs = 20000,
    TilerPowerTimeoutUs = 1000,
    ShaderOffPollUs = 1,
    TilerOffPollUs = 1,
    OffTimeoutUs = 2000
};

uint32_t lower32(uint64_t value)
{
    return static_cast<uint32_t>(value);
}

/* panfrost_get_core_mask(), panfrost_gpu.c:432-453: the first core group.
 * With the L2 bitmap as a contiguous low-bit mask, ~(l2 - 1) & (l2 - 2)
 * keeps only the bottom group. */
uint32_t coreMask(uint32_t l2Present)
{
    if(l2Present == 0)return 0;
    return ~(l2Present - 1) & (l2Present - 2);
}

uint32_t bits32(uint32_t value)
{
    uint32_t count = 0;
    while(value) {
        value &= value - 1;
        ++count;
    }
    return count;
}

uint32_t bits64(uint64_t value)
{
    uint32_t count = 0;
    while(value) {
        value &= value - 1;
        ++count;
    }
    return count;
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

/* readl_relaxed_poll_timeout(): read first, then sleep, and give up once the
 * budget is spent. A read or delay error is returned unchanged; only running
 * out of time is a timeout. */
IOReturn pollRawStatus(NativeMaliRegisterTarget *target, uint32_t mask,
    uint64_t timeout_us, uint32_t poll_us)
{
    uint32_t value = 0;
    uint64_t waited = 0;
    for(;;) {
        IOReturn result = readWord(target, GPU_INT_RAWSTAT, &value);
        if(result != kIOReturnSuccess)return result;
        if((value & mask) == mask)return kIOReturnSuccess;
        if(waited >= timeout_us)return kIOReturnTimeout;
        result = target->delay(poll_us);
        if(result != kIOReturnSuccess)return result;
        waited += poll_us;
    }
}

/* readl_poll_timeout() against a whole register. poll_us == 0 means "no
 * delay", as panfrost's L2 power-off poll asks for; without a free-running
 * clock in the IO seam, one immediate read is counted as one microsecond so
 * the budget still terminates. */
IOReturn pollExact(NativeMaliRegisterTarget *target, uint32_t offset,
    uint32_t expected, uint32_t poll_us, uint64_t timeout_us)
{
    uint32_t value = 0;
    uint64_t waited = 0;
    for(;;) {
        IOReturn result = readWord(target, offset, &value);
        if(result != kIOReturnSuccess)return result;
        if(value == expected)return kIOReturnSuccess;
        if(waited >= timeout_us)return kIOReturnTimeout;
        if(poll_us) {
            result = target->delay(poll_us);
            if(result != kIOReturnSuccess)return result;
            waited += poll_us;
        } else {
            ++waited;
        }
    }
}

/* LO | HI << 32, as panfrost_gpu.c does for every present bitmap. */
IOReturn readPresent(NativeMaliRegisterTarget *target, uint32_t low_offset,
    uint32_t high_offset, uint64_t *value)
{
    uint32_t low = 0;
    uint32_t high = 0;
    IOReturn result = readWord(target, low_offset, &low);
    if(result != kIOReturnSuccess)return result;
    result = readWord(target, high_offset, &high);
    if(result != kIOReturnSuccess)return result;
    *value = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
    return kIOReturnSuccess;
}

} // namespace

IOReturn NativeMaliGpuCore::prepare(NativeMaliRegisterTarget *gpu,
    NativeMaliPower *power)
{
    if(!gpu || !power)return kIOReturnBadArgument;
    if(gpu_ && (gpu_ != gpu || power_ != power))return kIOReturnBusy;
    if(!power->prepared())return kIOReturnNotReady;
    gpu_ = gpu;
    power_ = power;
    return kIOReturnSuccess;
}

IOReturn NativeMaliGpuCore::probe(NativeMaliGpuObservation *observation)
{
    if(!observation)return kIOReturnBadArgument;
    *observation = NativeMaliGpuObservation{};
    /* A failed or unsupported probe must not leave power-on using masks from
     * an older run. */
    observationValid_ = false;
    if(!prepared())return kIOReturnNotReady;

    /* The one rule that prevents a bus hang: do not touch 0x13040000 until M2
     * says the domains and gates are up. */
    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    /* Read the id first so a caller that sees a later read fail still has the
     * identity in the log. panfrost reads it after the feature words; the set
     * of registers is the same. */
    result = readWord(gpu_, GPU_ID, &observation->gpuId);
    if(result != kIOReturnSuccess)return result;
    const IOReturn identity = NativeMaliGpu::identify(observation->gpuId,
        &observation->identity);

    result = readWord(gpu_, GPU_L2_FEATURES, &observation->l2Features);
    if(result != kIOReturnSuccess)return result;
    result = readWord(gpu_, GPU_CORE_FEATURES, &observation->coreFeatures);
    if(result != kIOReturnSuccess)return result;
    result = readWord(gpu_, GPU_TILER_FEATURES, &observation->tilerFeatures);
    if(result != kIOReturnSuccess)return result;
    result = readWord(gpu_, GPU_MEM_FEATURES, &observation->memFeatures);
    if(result != kIOReturnSuccess)return result;
    result = readWord(gpu_, GPU_MMU_FEATURES, &observation->mmuFeatures);
    if(result != kIOReturnSuccess)return result;
    observation->ias = observation->mmuFeatures & 0xFF;
    observation->oas = (observation->mmuFeatures >> 8) & 0xFF;

    result = readWord(gpu_, GPU_AFBC_FEATURES, &observation->afbcFeatures);
    if(result != kIOReturnSuccess)return result;
    for(uint32_t index = 0; index < 4; ++index) {
        result = readWord(gpu_, GPU_TEXTURE_FEATURES(index),
            &observation->textureFeatures[index]);
        if(result != kIOReturnSuccess)return result;
    }

    result = readWord(gpu_, GPU_THREAD_FEATURES,
        &observation->threadFeatures);
    if(result != kIOReturnSuccess)return result;
    result = readWord(gpu_, GPU_THREAD_MAX_THREADS,
        &observation->maxThreads);
    if(result != kIOReturnSuccess)return result;
    result = readWord(gpu_, GPU_THREAD_MAX_WORKGROUP_SIZE,
        &observation->maxWorkgroupSize);
    if(result != kIOReturnSuccess)return result;
    result = readWord(gpu_, GPU_THREAD_MAX_BARRIER_SIZE,
        &observation->maxBarrierSize);
    if(result != kIOReturnSuccess)return result;

    result = readWord(gpu_, GPU_AS_PRESENT, &observation->asPresent);
    if(result != kIOReturnSuccess)return result;
    result = readWord(gpu_, GPU_JS_PRESENT, &observation->jsPresent);
    if(result != kIOReturnSuccess)return result;
    observation->jsCount = bits32(observation->jsPresent);
    /* panfrost's array is js_features[16] (panfrost_device.h:85). A mask with
     * more bits than that is not this hardware, so stop instead of reading
     * whatever follows in the page. */
    if(observation->jsCount > 16)return kIOReturnUnsupported;
    for(uint32_t index = 0; index < observation->jsCount; ++index) {
        result = readWord(gpu_, GPU_JS_FEATURES(index),
            &observation->jsFeatures[index]);
        if(result != kIOReturnSuccess)return result;
    }

    result = readPresent(gpu_, GPU_SHADER_PRESENT_LO, GPU_SHADER_PRESENT_HI,
        &observation->shaderPresent);
    if(result != kIOReturnSuccess)return result;
    result = readPresent(gpu_, GPU_TILER_PRESENT_LO, GPU_TILER_PRESENT_HI,
        &observation->tilerPresent);
    if(result != kIOReturnSuccess)return result;
    result = readPresent(gpu_, GPU_L2_PRESENT_LO, GPU_L2_PRESENT_HI,
        &observation->l2Present);
    if(result != kIOReturnSuccess)return result;
    result = readPresent(gpu_, GPU_STACK_PRESENT_LO, GPU_STACK_PRESENT_HI,
        &observation->stackPresent);
    if(result != kIOReturnSuccess)return result;

    result = readWord(gpu_, GPU_THREAD_TLS_ALLOC,
        &observation->threadTlsAlloc);
    if(result != kIOReturnSuccess)return result;

    observation->maxTasksPerCore = observation->threadFeatures >> 24;
    if(observation->maxTasksPerCore == 0)observation->maxTasksPerCore = 1;
    observation->registersPerCore = observation->threadFeatures & 0xFFFF;
    if(observation->registersPerCore == 0) {
        /* panfrost's arch table: v7 assumes 32 registers per thread. It is a
         * fallback for a register that reads zero, not a hardware value. */
        observation->registersPerCore = observation->maxThreads * 32;
    }
    observation->tlsInstancesPerCore = observation->threadTlsAlloc;
    if(observation->tlsInstancesPerCore == 0)
        observation->tlsInstancesPerCore = observation->maxThreads;

    observation->coreCount = bits64(observation->shaderPresent);
    observation->tilerCount = bits64(observation->tilerPresent);
    observation->l2Count = bits64(observation->l2Present);
    observation->coreGroups = bits64(observation->l2Present);
    observation->stackCount = bits64(observation->stackPresent);

    /* Power masks come from a fully read, known model with a complete probe. */
    if(identity == kIOReturnSuccess) {
        observation_ = *observation;
        observationValid_ = true;
    }
    return identity;
}

IOReturn NativeMaliGpuCore::softReset(uint64_t timeout_us)
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;
    resetDone_ = false;

    /* panfrost_gpu_soft_reset(), panfrost_gpu.c:62-108. */
    resetMark(5, "R1 mask+clear");
    result = writeWord(gpu_, GPU_INT_MASK, 0);
    if(result != kIOReturnSuccess)return result;
    result = writeWord(gpu_, GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
    if(result != kIOReturnSuccess)return result;
    resetMark(6, "R2 cmd");
    result = writeWord(gpu_, GPU_CMD, GPU_CMD_SOFT_RESET);
    if(result != kIOReturnSuccess)return result;

    resetMark(7, "R3 poll");
    result = pollRawStatus(gpu_, GPU_IRQ_RESET_COMPLETED, timeout_us,
        SoftResetPollUs);
    if(result == kIOReturnTimeout) {
        /* The source falls back to a hard reset on timeout. Do the same, then
         * report a failed hard reset unchanged; leaving the mask at zero is
         * the safest state for a caller that must stop. */
        result = writeWord(gpu_, GPU_CMD, GPU_CMD_HARD_RESET);
        if(result != kIOReturnSuccess)return result;
        resetMark(8, "R4 hard reset");
        result = pollRawStatus(gpu_, GPU_IRQ_RESET_COMPLETED, timeout_us,
            HardResetPollUs);
    }
    if(result != kIOReturnSuccess)return result;

    resetMark(8, "R5 clear+mask");
    result = writeWord(gpu_, GPU_INT_CLEAR, GPU_IRQ_MASK_ALL);
    if(result != kIOReturnSuccess)return result;
    result = writeWord(gpu_, GPU_INT_MASK, GPU_IRQ_MASK_ERROR |
        GPU_IRQ_PERFCNT_SAMPLE_COMPLETED | GPU_IRQ_CLEAN_CACHES_COMPLETED);
    if(result == kIOReturnSuccess)resetDone_ = true;
    return result;
}

IOReturn NativeMaliGpuCore::readFault(NativeMaliGpuFault *fault)
{
    if(!fault)return kIOReturnBadArgument;
    *fault = NativeMaliGpuFault{};
    if(!prepared())return kIOReturnNotReady;

    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    /* panfrost_gpu_irq_handler(), panfrost_gpu.c:32-35: the status, then the
     * high word, then the low word. */
    result = readWord(gpu_, GPU_FAULT_STATUS, &fault->status);
    if(result != kIOReturnSuccess)return result;
    result = readWord(gpu_, GPU_FAULT_ADDRESS_HI, &fault->addressHi);
    if(result != kIOReturnSuccess)return result;
    return readWord(gpu_, GPU_FAULT_ADDRESS_LO, &fault->addressLo);
}

IOReturn NativeMaliGpuCore::latestFlushId(uint32_t *flushId)
{
    if(!flushId)return kIOReturnBadArgument;
    *flushId = 0;
    if(!prepared())return kIOReturnNotReady;

    /* panfrost_gpu_get_latest_flush_id(), panfrost_gpu.c:561-573: a single
     * GPU_LATEST_FLUSH_ID read, only while the GPU is in use. The gate failure
     * is returned rather than turned into the source's 0, because 0 is a valid
     * flush id and the caller cannot tell the two apart. */
    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;
    return readWord(gpu_, GPU_LATEST_FLUSH_ID, flushId);
}

IOReturn NativeMaliGpuCore::initQuirks()
{
    /* panfrost_gpu_init_quirks(), panfrost_gpu.c:117-180. The SC_* bits
     * (8443, 11035, 10327, 10797, TTRX_2968_TTRX_3162) and
     * TC_CLOCK_GATE_OVERRIDE (T76X_3953) are all guarded by issues G52 does not
     * have, and the model comparisons are for T60x..T8xx, so the shader config
     * stays untouched. HW_FEATURE_IDVS_GROUP_SIZE is in the G52 feature set,
     * which makes this the one quirk write. */
    const uint32_t jmConfig =
        JM_DEFAULT_IDVS_GROUP_SIZE << JM_IDVS_GROUP_SIZE_SHIFT;
    return writeWord(gpu_, GPU_JM_CONFIG, jmConfig);
}

IOReturn NativeMaliGpuCore::powerWait(uint32_t offset, uint32_t expected,
    uint32_t poll_us, uint64_t timeout_us)
{
    return pollExact(gpu_, offset, expected, poll_us, timeout_us);
}

IOReturn NativeMaliGpuCore::powerMasks(uint32_t *l2, uint32_t *shader,
    uint32_t *tiler) const
{
    /* Only the LO halves are written, as the source does. A mask with a high
     * bit would silently leave that core unpowered, so refuse instead. */
    if((observation_.l2Present >> 32) || (observation_.shaderPresent >> 32) ||
        (observation_.tilerPresent >> 32))
        return kIOReturnUnsupported;
    const uint32_t l2Value = lower32(observation_.l2Present);
    const uint32_t shaderValue = lower32(observation_.shaderPresent);
    const uint32_t tilerValue = lower32(observation_.tilerPresent);
    if(!l2Value || !shaderValue || !tilerValue)return kIOReturnUnsupported;
    *l2 = l2Value;
    *shader = shaderValue;
    *tiler = tilerValue;
    return kIOReturnSuccess;
}

IOReturn NativeMaliGpuCore::powerOn()
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    if(!observationValid_ || !resetDone_)return kIOReturnNotReady;

    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    uint32_t l2 = 0;
    uint32_t shader = 0;
    uint32_t tiler = 0;
    result = powerMasks(&l2, &shader, &tiler);
    if(result != kIOReturnSuccess)return result;
    const uint32_t cores = coreMask(l2);
    if(!cores)return kIOReturnUnsupported;
    const uint32_t l2Mask = l2 & cores;
    const uint32_t shaderMask = shader & cores;

    /* panfrost_gpu_power_on() starts with the quirks (panfrost_gpu.c:461).
     * Nothing is powered yet, so a failure here needs no rollback. */
    result = initQuirks();
    if(result != kIOReturnSuccess)return result;

    /* panfrost_gpu_power_on(), panfrost_gpu.c:455-484. A failed step quiesces
     * everything that may have been switched on and returns the first error. */
    result = writeWord(gpu_, L2_PWRON_LO, l2Mask);
    if(result == kIOReturnSuccess)
        result = powerWait(L2_READY_LO, l2Mask, L2PowerPollUs,
            L2PowerTimeoutUs);
    if(result != kIOReturnSuccess) {
        (void)powerOffSteps();
        resetDone_ = false;
        return result;
    }

    result = writeWord(gpu_, SHADER_PWRON_LO, shaderMask);
    if(result == kIOReturnSuccess)
        result = powerWait(SHADER_READY_LO, shaderMask, ShaderPowerPollUs,
            ShaderPowerTimeoutUs);
    if(result != kIOReturnSuccess) {
        (void)powerOffSteps();
        resetDone_ = false;
        return result;
    }

    result = writeWord(gpu_, TILER_PWRON_LO, tiler);
    if(result == kIOReturnSuccess)
        result = powerWait(TILER_READY_LO, tiler, TilerPowerPollUs,
            TilerPowerTimeoutUs);
    if(result != kIOReturnSuccess) {
        (void)powerOffSteps();
        resetDone_ = false;
        return result;
    }
    return kIOReturnSuccess;
}

IOReturn NativeMaliGpuCore::powerOffSteps()
{
    uint32_t l2 = 0;
    uint32_t shader = 0;
    uint32_t tiler = 0;
    IOReturn result = powerMasks(&l2, &shader, &tiler);
    if(result != kIOReturnSuccess)return result;
    IOReturn first = kIOReturnSuccess;

    /* panfrost_gpu_power_off(), panfrost_gpu.c:486-504: shader, tiler, L2.
     * Each block is attempted even if an earlier transition failed. */
    result = writeWord(gpu_, SHADER_PWROFF_LO, shader);
    if(result == kIOReturnSuccess)
        result = powerWait(SHADER_PWRTRANS_LO, 0, ShaderOffPollUs,
            OffTimeoutUs);
    if(result != kIOReturnSuccess && first == kIOReturnSuccess)first = result;

    result = writeWord(gpu_, TILER_PWROFF_LO, tiler);
    if(result == kIOReturnSuccess)
        result = powerWait(TILER_PWRTRANS_LO, 0, TilerOffPollUs, OffTimeoutUs);
    if(result != kIOReturnSuccess && first == kIOReturnSuccess)first = result;

    result = writeWord(gpu_, L2_PWROFF_LO, l2);
    if(result == kIOReturnSuccess)
        result = powerWait(L2_PWRTRANS_LO, 0, 0, OffTimeoutUs);
    if(result != kIOReturnSuccess && first == kIOReturnSuccess)first = result;

    return first;
}

IOReturn NativeMaliGpuCore::powerOff()
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    if(!observationValid_)return kIOReturnNotReady;

    IOReturn result = power_->gpuReadable();
    if(result != kIOReturnSuccess)return result;

    result = powerOffSteps();
    /* Whether or not every transition settled, the next power-up must start
     * with a reset. */
    resetDone_ = false;
    return result;
}
