/* SPDX-License-Identifier: GPL-2.0-only */
/* The client body; the rules and their sources are in NativeMaliUserClient.h. */
#include "NativeMaliUserClient.h"
#include "NativeMaliJob.h"
#include "SprdMaliJobRouting.h"
#include <string.h>

namespace {

uint32_t lower32(uint64_t value)
{
    return static_cast<uint32_t>(value);
}

uint32_t upper32(uint64_t value)
{
    return static_cast<uint32_t>(value >> 32);
}

} // namespace

uint64_t NativeMaliUserClient::timeoutMicroseconds(uint64_t timeout_ns)
{
    /* The ABI's 0 is "poll once", which a zero budget already does. A nonzero
     * budget rounds up to 1 us so a sub-microsecond wait still polls. */
    if(!timeout_ns)return 0;
    return timeout_ns / 1000 + (timeout_ns % 1000 != 0);
}

IOReturn NativeMaliUserClient::open(NativeMaliMemorySource *source,
    NativeMaliCacheMaintainer *maintainer, bool cpuCoherent,
    NativeMaliVm *vm, NativeMaliMmu *mmu, uint32_t as,
    NativeMaliJobSlot *jobs, NativeMaliGpuCore *gpu,
    const NativeMaliGpuObservation &observation, uint64_t maxBoBytes,
    NativeMaliResetTarget *reset)
{
    if(open_)return kIOReturnBusy;
    if(!vm || !jobs || !gpu)return kIOReturnBadArgument;

    const uint64_t cap = maxBoBytes ? maxBoBytes :
        static_cast<uint64_t>(NATIVE_MALI_ABI_MAX_BO_BYTES);
    const IOReturn result = bo_.open(source, maintainer, cpuCoherent, vm, mmu,
        as, cap);
    if(result != kIOReturnSuccess)return result;

    jobs_ = jobs;
    gpu_ = gpu;
    vm_ = vm;
    reset_ = reset;
    observation_ = observation;
    as_ = as;
    maxBoBytes_ = cap;
    for(uint32_t &seqno : submitted_)seqno = 0;
    for(uint32_t &seqno : deadSeqno_)seqno = 0;
    pendingSync_ = NativeMaliFence{};
    nextSlot_ = 0;
    faults_ = 0;
    resets_ = 0;
    open_ = true;
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::allocBo(const NativeMaliAbiBoAlloc &request,
    NativeMaliAbiBoAllocReply *reply)
{
    return bo_.alloc(request, reply);
}

IOReturn NativeMaliUserClient::mapBo(const NativeMaliAbiBoMap &request,
    NativeMaliAbiBoMapReply *reply)
{
    if(hasOutstanding())return kIOReturnBusy;
    return bo_.map(request, reply);
}

IOReturn NativeMaliUserClient::freeBo(const NativeMaliAbiBoFree &request)
{
    if(hasOutstanding())return kIOReturnBusy;
    return bo_.freeBo(request);
}

IOReturn NativeMaliUserClient::cpuMap(const NativeMaliAbiBoCpuMap &request,
    NativeMaliAbiBoCpuMapReply *reply)
{
    return bo_.cpuMap(request, reply);
}

IOReturn NativeMaliUserClient::cpuUnmap(const NativeMaliAbiBoCpuUnmap &request)
{
    return bo_.cpuUnmap(request);
}

IOReturn NativeMaliUserClient::query(const NativeMaliAbiQuery &request,
    NativeMaliAbiQuery *reply)
{
    if(!reply)return kIOReturnBadArgument;
    /* Validate before zeroing, so a caller that passed one buffer as both
     * records still has its request read. */
    const IOReturn result = NativeMaliAbiValidate::query(request);
    *reply = NativeMaliAbiQuery{};
    if(result != kIOReturnSuccess)return result;
    if(!open_)return kIOReturnNotReady;

    reply->abi_version = request.abi_version;
    reply->input_bits = vm_->info().ias;
    reply->output_bits = vm_->info().oas;
    reply->flags = 0;
    reply->max_bo_bytes = maxBoBytes_;
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::deviceInfo(NativeMaliAbiDeviceInfo *reply)
{
    if(!reply)return kIOReturnBadArgument;
    *reply = NativeMaliAbiDeviceInfo{};
    if(!open_)return kIOReturnNotReady;

    reply->gpu_id = observation_.gpuId;
    reply->shader_present_lo = lower32(observation_.shaderPresent);
    reply->shader_present_hi = upper32(observation_.shaderPresent);
    reply->tiler_present_lo = lower32(observation_.tilerPresent);
    reply->tiler_present_hi = upper32(observation_.tilerPresent);
    reply->thread_max_threads = observation_.maxThreads;
    reply->max_workgroup_size = observation_.maxWorkgroupSize;
    reply->max_barrier_size = observation_.maxBarrierSize;
    reply->l2_features = observation_.l2Features;
    reply->core_features = observation_.coreFeatures;
    reply->tiler_features = observation_.tilerFeatures;
    reply->flags = bo_.coherent() ? 0 : NativeMaliDeviceNonCoherent;
    reply->va_start = 0;
    reply->va_size = UINT64_C(1) << vm_->info().ias;

    /* v2 fields. A v1 caller receives only the first 64 bytes, which are the
     * same layout; dispatch copies min(requested, sizeof). */
    reply->mem_features = observation_.memFeatures;
    reply->mmu_features = observation_.mmuFeatures;
    reply->afbc_features = observation_.afbcFeatures;
    for(uint32_t index = 0; index < 4; ++index)
        reply->texture_features[index] = observation_.textureFeatures[index];
    reply->max_tasks_per_core = observation_.maxTasksPerCore;
    reply->registers_per_core = observation_.registersPerCore;
    reply->tls_instances_per_core = observation_.tlsInstancesPerCore;
    reply->pgsize_shift = NativeMaliAbiPageShift;
    reply->vm_op_flags = NativeMaliAbiVmOpMap | NativeMaliAbiVmOpUnmap;
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::getParam(const NativeMaliAbiParam &request,
    NativeMaliAbiParam *reply)
{
    if(!reply || request.reserved || request.value)return kIOReturnBadArgument;
    if(!open_)return kIOReturnNotReady;
    uint64_t value=0;
    switch(request.parameter) {
    case 0: value=observation_.gpuId>>16; break;
    case 1: value=observation_.gpuId&0xffff; break;
    case 2: value=observation_.shaderPresent; break;
    case 3: value=observation_.tilerPresent; break;
    case 4: value=observation_.l2Present; break;
    case 5: value=observation_.stackPresent; break;
    case 6: value=observation_.asPresent; break;
    case 7: value=observation_.jsPresent; break;
    case 8: value=observation_.l2Features; break;
    case 9: value=observation_.coreFeatures; break;
    case 10: value=observation_.tilerFeatures; break;
    case 11: value=observation_.memFeatures; break;
    case 12: value=observation_.mmuFeatures; break;
    case 13: value=observation_.threadFeatures; break;
    case 14: value=observation_.maxThreads; break;
    case 15: value=observation_.maxWorkgroupSize; break;
    case 16: value=observation_.maxBarrierSize; break;
    case 38: value=observation_.coreGroups; break;
    case 39: value=observation_.threadTlsAlloc; break;
    case 40: value=observation_.afbcFeatures; break;
    default:
        if(request.parameter>=18 && request.parameter<=21)
            value=observation_.textureFeatures[request.parameter-18];
        else if(request.parameter>=22 && request.parameter<=37)
            value=observation_.jsFeatures[request.parameter-22];
        else return kIOReturnUnsupported;
    }
    *reply={request.parameter,0,value};
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::cacheSync(const NativeMaliAbiBoCacheSync &request)
{
    if(!open_)return kIOReturnNotReady;
    if(request.reserved || request.type>1 || !request.bytes)
        return kIOReturnBadArgument;
    if(hasOutstanding())return kIOReturnBusy;
    auto *memory=bo_.memory(request.handle);
    if(!memory || request.offset>=memory->bytes() ||
        request.bytes>memory->bytes()-request.offset)return kIOReturnBadArgument;
    IOReturn result=memory->cleanForDevice(request.offset,request.bytes);
    if(result==kIOReturnSuccess && request.type==1)
        result=memory->invalidateForCpu(request.offset,request.bytes);
    return result;
}

bool NativeMaliUserClient::hasOutstanding() const
{
    if(pendingSync_.valid())return true;
    for(uint32_t js=0;js<NativeMaliJobSlotCount;++js)
        if(ownOutstanding(js))return true;
    return false;
}

IOReturn NativeMaliUserClient::chooseSlot(uint64_t chain, uint32_t *slot)
{
    if(!slot)return kIOReturnBadArgument;
    uint32_t selected=UINT32_MAX;
    // Every linked header must be in this AS and belong on the same queue.
    // A finite walk rejects loops without trusting user-controlled Next fields.
    for(unsigned count=0;chain && count<256;++count) {
        if(chain & (NativeMaliJobHeadAlignment-1))return kIOReturnBadArgument;
        alignas(64) uint8_t packed[64]={};
        IOReturn result=bo_.readMapped(chain,packed,sizeof(packed));
        if(result!=kIOReturnSuccess)return result;
        NativeMaliJobHeader header;
        result=NativeMaliJobChain::unpackHeader(packed,&header);
        if(result!=kIOReturnSuccess)return result;
        uint32_t required=0;
        if(!SprdMaliJobRouting::choose(jobs_->present(),
            static_cast<SprdMaliJobRouting::Type>(header.type),&required))
            return kIOReturnUnsupported;
        if(selected!=UINT32_MAX && selected!=required)return kIOReturnBadArgument;
        selected=required;
        chain=header.next;
        if(!chain) {
            if(jobs_->faulted(selected))return kIOReturnNotReady;
            if(jobs_->outstanding(selected))return kIOReturnBusy;
            *slot=selected;
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

bool NativeMaliUserClient::ownOutstanding(uint32_t js) const
{
    return js < NativeMaliJobSlotCount && jobs_->outstanding(js) &&
        jobs_->latestSeqno(js) <= submitted_[js];
}

IOReturn NativeMaliUserClient::finishCpuSync()
{
    const IOReturn result=bo_.maintainMapped(false);
    if(result==kIOReturnSuccess)pendingSync_=NativeMaliFence{};
    return result;
}

IOReturn NativeMaliUserClient::waitForFence(const NativeMaliFence &fence,
    uint64_t timeout_ns, NativeMaliJobCompletion *completion)
{
    if(!open_)return kIOReturnNotReady;
    if(!fence.valid() || fence.js >= NativeMaliJobSlotCount)
        return kIOReturnBadArgument;
    /* Only a fence this client was handed can be waited on. A higher seqno is
     * another client's newest job (or one not submitted yet), and reaping it
     * here would steal its completion and hide its error. */
    if(fence.seqno > submitted_[fence.js])return kIOReturnBadArgument;
    if(deadSeqno_[fence.js] == fence.seqno)return kIOReturnIOError;
    if(pendingSync_.valid() && pendingSync_.js == fence.js &&
        pendingSync_.seqno == fence.seqno)return finishCpuSync();
    /* An older fence is a query. Invalidating here would cross ownership
     * into a newer job or discard CPU writes made since the earlier sync. */
    if(jobs_->fenceSignaled(fence))return kIOReturnSuccess;

    /* Not signaled, so this must be the slot's newest submission, and it must
     * still be outstanding. Anything else is a bookkeeping bug; refuse rather
     * than wait on a slot that could belong to someone else. */
    if(jobs_->latestSeqno(fence.js) != fence.seqno)return kIOReturnNotReady;
    if(!jobs_->outstanding(fence.js))return kIOReturnNotReady;

    NativeMaliJobCompletion local;
    if(completion)*completion = NativeMaliJobCompletion{};
    const IOReturn result = jobs_->waitForCompletion(fence.js,
        completion ? completion : &local, timeoutMicroseconds(timeout_ns),
        CompletionPollUs);
    if(result == kIOReturnSuccess) {
        pendingSync_=fence;
        return finishCpuSync();
    }
    if(result == kIOReturnIOError && !jobs_->outstanding(fence.js)) {
        deadSeqno_[fence.js]=fence.seqno;
        if((completion ? completion : &local)->isFault())++faults_;
    }
    return result;
}

IOReturn NativeMaliUserClient::submit(const NativeMaliAbiSubmit &request,
    NativeMaliAbiSubmitReply *reply)
{
    if(!reply)return kIOReturnBadArgument;
    reply->fence = 0;
    if(!open_)return kIOReturnNotReady;

    IOReturn result = NativeMaliAbiValidate::submit(request);
    if(result != kIOReturnSuccess)return result;
    /* The chain head must be an address this client mapped: the hardware will
     * fetch 64 bytes there, and an unmapped address is a GPU fault. */
    if(!bo_.covers(request.job_chain, NativeMaliJobHeadAlignment))
        return kIOReturnBadArgument;

    if(request.wait_fence) {
        const NativeMaliFence dependency{
            NativeMaliAbiValidate::fenceSlot(request.wait_fence),
            NativeMaliAbiValidate::fenceSeqno(request.wait_fence)};
        result = waitForFence(dependency, request.wait_timeout_ns, nullptr);
        if(result != kIOReturnSuccess)return result;
    }

    uint32_t slot = 0;
    if(hasOutstanding())return kIOReturnBusy;
    result=bo_.maintainMapped(true);
    if(result!=kIOReturnSuccess)return result;
    result=chooseSlot(request.job_chain,&slot);
    if(result!=kIOReturnSuccess)return result;

    uint32_t flushId = 0;
    result = gpu_->latestFlushId(&flushId);
    if(result != kIOReturnSuccess)return result;

    NativeMaliFence fence{};
    result = jobs_->submit(slot, request.job_chain, as_, flushId, &fence);
    if(result != kIOReturnSuccess)return result;

    submitted_[slot] = fence.seqno;
    nextSlot_ = (slot + 1) % NativeMaliJobSlotCount;
    reply->fence = NativeMaliAbiValidate::packFence(fence.js, fence.seqno);
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::waitFence(const NativeMaliAbiWaitFence &request)
{
    if(!open_)return kIOReturnNotReady;
    const IOReturn result = NativeMaliAbiValidate::waitFence(request);
    if(result != kIOReturnSuccess)return result;
    const NativeMaliFence fence{
        NativeMaliAbiValidate::fenceSlot(request.fence),
        NativeMaliAbiValidate::fenceSeqno(request.fence)};
    if(request.timeout_ns>1000000000ULL)return kIOReturnBadArgument;
    return waitForFence(fence, request.timeout_ns, nullptr);
}

IOReturn NativeMaliUserClient::status(NativeMaliAbiStatus *reply)
{
    if(!reply)return kIOReturnBadArgument;
    *reply = NativeMaliAbiStatus{};
    if(!open_)return kIOReturnNotReady;
    reply->faults = faults_;
    reply->resets = resets_;
    return kIOReturnSuccess;
}

size_t NativeMaliUserClient::inputSize(uint32_t method)
{
    switch(method) {
    case NativeMaliMethodQuery: return sizeof(NativeMaliAbiQuery);
    case NativeMaliMethodGetDeviceInfo: return 0;
    case NativeMaliMethodBoAlloc: return sizeof(NativeMaliAbiBoAlloc);
    case NativeMaliMethodBoMap: return sizeof(NativeMaliAbiBoMap);
    case NativeMaliMethodBoFree: return sizeof(NativeMaliAbiBoFree);
    case NativeMaliMethodBoCpuMap: return sizeof(NativeMaliAbiBoCpuMap);
    case NativeMaliMethodBoCpuUnmap: return sizeof(NativeMaliAbiBoCpuUnmap);
    case NativeMaliMethodSubmit: return sizeof(NativeMaliAbiSubmit);
    case NativeMaliMethodWaitFence: return sizeof(NativeMaliAbiWaitFence);
    case NativeMaliMethodGetStatus: return 0;
    case NativeMaliMethodReset: return 0;
    case NativeMaliMethodGetParam: return sizeof(NativeMaliAbiParam);
    case NativeMaliMethodBoCacheSync: return sizeof(NativeMaliAbiBoCacheSync);
    default: return kUnsupportedSize;
    }
}

size_t NativeMaliUserClient::outputSize(uint32_t method)
{
    switch(method) {
    case NativeMaliMethodQuery: return sizeof(NativeMaliAbiQuery);
    case NativeMaliMethodGetDeviceInfo: return sizeof(NativeMaliAbiDeviceInfo);
    case NativeMaliMethodBoAlloc: return sizeof(NativeMaliAbiBoAllocReply);
    case NativeMaliMethodBoMap: return sizeof(NativeMaliAbiBoMapReply);
    case NativeMaliMethodBoFree: return 0;
    case NativeMaliMethodBoCpuMap: return sizeof(NativeMaliAbiBoCpuMapReply);
    case NativeMaliMethodBoCpuUnmap: return 0;
    case NativeMaliMethodSubmit: return sizeof(NativeMaliAbiSubmitReply);
    case NativeMaliMethodWaitFence: return 0;
    case NativeMaliMethodGetStatus: return sizeof(NativeMaliAbiStatus);
    case NativeMaliMethodReset: return 0;
    case NativeMaliMethodGetParam: return sizeof(NativeMaliAbiParam);
    case NativeMaliMethodBoCacheSync: return 0;
    default: return kUnsupportedSize;
    }
}

IOReturn NativeMaliUserClient::dispatch(uint32_t method, const void *input,
    size_t input_size, void *output, size_t output_size)
{
    const size_t expected_in = inputSize(method);
    const size_t expected_out = outputSize(method);
    if(expected_in == kUnsupportedSize || expected_out == kUnsupportedSize)
        return kIOReturnUnsupported;
    if(input_size != expected_in)
        return kIOReturnBadArgument;
    /* GetDeviceInfo accepts the v1 record as well: v2 is a strict prefix
     * extension, and the caller's buffer size says which it wants. */
    if(method == NativeMaliMethodGetDeviceInfo) {
        if(output_size != NativeMaliAbiDeviceInfoV1Bytes &&
            output_size != expected_out)
            return kIOReturnBadArgument;
    } else if(output_size != expected_out) {
        return kIOReturnBadArgument;
    }
    if((expected_in != 0 && !input) || (expected_out != 0 && !output))
        return kIOReturnBadArgument;

    /* Every reply is built in a zeroed local and copied out only after the
     * typed method succeeds; a refusal leaves the caller's buffer untouched. */
    switch(method) {
    case NativeMaliMethodQuery: {
        NativeMaliAbiQuery request{};
        memcpy(&request, input, sizeof(request));
        NativeMaliAbiQuery reply{};
        const IOReturn result = query(request, &reply);
        if(result != kIOReturnSuccess)return result;
        memcpy(output, &reply, sizeof(reply));
        return kIOReturnSuccess;
    }
    case NativeMaliMethodGetDeviceInfo: {
        NativeMaliAbiDeviceInfo reply{};
        const IOReturn result = deviceInfo(&reply);
        if(result != kIOReturnSuccess)return result;
        /* v1 callers asked for 64 bytes; copy exactly what they asked for. */
        memcpy(output, &reply, output_size);
        return kIOReturnSuccess;
    }
    case NativeMaliMethodBoAlloc: {
        NativeMaliAbiBoAlloc request{};
        memcpy(&request, input, sizeof(request));
        NativeMaliAbiBoAllocReply reply{};
        const IOReturn result = allocBo(request, &reply);
        if(result != kIOReturnSuccess)return result;
        memcpy(output, &reply, sizeof(reply));
        return kIOReturnSuccess;
    }
    case NativeMaliMethodBoMap: {
        NativeMaliAbiBoMap request{};
        memcpy(&request, input, sizeof(request));
        NativeMaliAbiBoMapReply reply{};
        const IOReturn result = mapBo(request, &reply);
        if(result != kIOReturnSuccess)return result;
        memcpy(output, &reply, sizeof(reply));
        return kIOReturnSuccess;
    }
    case NativeMaliMethodBoFree: {
        NativeMaliAbiBoFree request{};
        memcpy(&request, input, sizeof(request));
        return freeBo(request);
    }
    case NativeMaliMethodBoCpuMap: {
        NativeMaliAbiBoCpuMap request{};
        memcpy(&request, input, sizeof(request));
        NativeMaliAbiBoCpuMapReply reply{};
        const IOReturn result = cpuMap(request, &reply);
        if(result != kIOReturnSuccess)return result;
        memcpy(output, &reply, sizeof(reply));
        return kIOReturnSuccess;
    }
    case NativeMaliMethodBoCpuUnmap: {
        NativeMaliAbiBoCpuUnmap request{};
        memcpy(&request, input, sizeof(request));
        return cpuUnmap(request);
    }
    case NativeMaliMethodSubmit: {
        NativeMaliAbiSubmit request{};
        memcpy(&request, input, sizeof(request));
        NativeMaliAbiSubmitReply reply{};
        const IOReturn result = submit(request, &reply);
        if(result != kIOReturnSuccess)return result;
        memcpy(output, &reply, sizeof(reply));
        return kIOReturnSuccess;
    }
    case NativeMaliMethodWaitFence: {
        NativeMaliAbiWaitFence request{};
        memcpy(&request, input, sizeof(request));
        return waitFence(request);
    }
    case NativeMaliMethodGetStatus: {
        NativeMaliAbiStatus reply{};
        const IOReturn result = status(&reply);
        if(result != kIOReturnSuccess)return result;
        memcpy(output, &reply, sizeof(reply));
        return kIOReturnSuccess;
    }
    case NativeMaliMethodGetParam: {
        NativeMaliAbiParam request{},reply{};
        memcpy(&request,input,sizeof(request));
        const IOReturn result=getParam(request,&reply);
        if(result!=kIOReturnSuccess)return result;
        memcpy(output,&reply,sizeof(reply));
        return kIOReturnSuccess;
    }
    case NativeMaliMethodBoCacheSync: {
        NativeMaliAbiBoCacheSync request{};
        memcpy(&request,input,sizeof(request));
        return cacheSync(request);
    }
    case NativeMaliMethodReset:
        /* The target is the device-wide owner; this client cannot reset the
         * GPU by itself, and a missing target is not a silent success. */
        if(!open_)return kIOReturnNotReady;
        return reset_ ? reset_->resetDevice() : kIOReturnUnsupported;
    default:
        /* inputSize()/outputSize() already rejected every other selector. */
        return kIOReturnUnsupported;
    }
}

void NativeMaliUserClient::noteReset()
{
    if(!open_)return;
    /* latest > completed means the job was never reaped, so the reset killed
     * it; `<= submitted_` means it was this client's job and not another
     * client's later submission on the shared slot. Keep the seqno: a later
     * submission on the same slot gets a higher one, so this can only ever
     * match the lost job.
     *
     * A reset that kills nothing on a slot must not clear an older marker.
     * completedSeqno_ only moves forward, so a fence this slot lost once would
     * otherwise later look like an ordinary reaped fence and answer Success. */
    for(uint32_t js = 0; js < NativeMaliJobSlotCount; ++js) {
        const uint32_t latest = jobs_->latestSeqno(js);
        if(latest > jobs_->completedSeqno(js) && latest <= submitted_[js])
            deadSeqno_[js] = latest;
    }
    ++resets_;
}

IOReturn NativeMaliUserClient::abandon()
{
    if(!open_)return kIOReturnNotReady;
    /* panfrost_mmu_release_ctx() does not disable an address space whose
     * device is not runtime-active (panfrost_mmu.c:731-732). The service makes
     * the same decision for the same reason: an unreadable block cannot still
     * walk this client's tables, so the leaves and the flush are skipped and
     * the buffers go straight back to their source. */
    bo_.reset();
    open_ = false;
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::close()
{
    if(!open_)return kIOReturnNotReady;
    /* Live objects or unobserved jobs are still owned by userspace; closing
     * would retire them behind its back. */
    if(bo_.live() != 0)return kIOReturnBusy;
    for(uint32_t js = 0; js < NativeMaliJobSlotCount; ++js)
        if(ownOutstanding(js))return kIOReturnBusy;

    bo_.reset();
    open_ = false;
    return kIOReturnSuccess;
}

IOReturn NativeMaliUserClient::releaseAll(uint64_t timeout_us)
{
    if(!open_)return kIOReturnNotReady;

    /* Wait for this client's jobs first, so no chain can still read a buffer
     * that is about to be unmapped. A slotted job that is not ours is left
     * alone: another client owns its completion. */
    for(uint32_t js = 0; js < NativeMaliJobSlotCount; ++js) {
        if(!ownOutstanding(js))continue;
        NativeMaliJobCompletion completion;
        const IOReturn repeated = jobs_->waitForCompletion(js, &completion,
            timeout_us, CompletionPollUs);
        if(repeated == kIOReturnIOError && completion.isFault())++faults_;
        if(repeated != kIOReturnSuccess)return repeated;
    }

    const IOReturn result = bo_.releaseAll();
    if(result != kIOReturnSuccess)return result;
    bo_.reset();
    open_ = false;
    return kIOReturnSuccess;
}
