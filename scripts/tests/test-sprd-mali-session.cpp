// SPDX-License-Identifier: GPL-2.0-only
// Run actual BO/client/MMU/job code against powered register and allocation doubles.
#include "NativeMaliUserClient.h"
#include "NativeMaliJob.h"
#include "NativeMaliRegs.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <cstdio>
extern "C" void sprd_mali_trace_mark(unsigned,const char *) {}

class RegisterFile final : public NativeMaliRegisterTarget {
public:
    uint32_t data[0x4000/4]={};
    std::vector<unsigned> submitted;
    bool completeOnStart=true;
    void complete(unsigned js) {data[JOB_INT_RAWSTAT/4]|=JOB_INT_MASK_DONE(js);}
    IOReturn read32(uint32_t at,uint32_t *v) override {
        if(!v || at>=sizeof(data) || (at&3))return kIOReturnBadArgument;
        *v=data[at/4];return kIOReturnSuccess;
    }
    IOReturn write32(uint32_t at,uint32_t v) override {
        if(at>=sizeof(data) || (at&3))return kIOReturnBadArgument;
        if(at==JOB_INT_CLEAR)data[JOB_INT_RAWSTAT/4]&=~v;
        else data[at/4]=v;
        for(unsigned js=0;js<3;++js)if(at==JS_COMMAND_NEXT(js) && v==JS_COMMAND_START) {
            submitted.push_back(js); data[at/4]=0;
            if(completeOnStart)complete(js);
        }
        return kIOReturnSuccess;
    }
    IOReturn delay(uint64_t) override {return kIOReturnSuccess;}
};
class Pages final : public NativeMaliPageSource {
    uint64_t next_=0x20000000;
public:
    unsigned live=0;
    IOReturn alloc(NativeMaliPage *p) override {
        auto *cpu=std::aligned_alloc(4096,4096);
        if(!cpu)return kIOReturnNoMemory;
        *p={cpu,next_,4096};next_+=4096;++live;
        return kIOReturnSuccess;
    }
    void release(const NativeMaliPage &p) override {std::free(p.cpu);--live;}
};
class Buffers final : public NativeMaliMemorySource {
    uint64_t next_=0x30000000;
public:
    unsigned live=0;
    IOReturn alloc(size_t bytes,void **cpu,NativeMaliMemorySpan *spans,size_t *count) override {
        *cpu=std::aligned_alloc(4096,bytes);
        if(!*cpu)return kIOReturnNoMemory;
        memset(*cpu,0,bytes);spans[0]={0,next_,bytes};next_+=bytes;
        *count=1;++live;return kIOReturnSuccess;
    }
    void release(void *cpu,const NativeMaliMemorySpan *,size_t) override {
        std::free(cpu);--live;
    }
};
class Cache final : public NativeMaliCacheMaintainer {
public:
    unsigned clean=0,invalidate=0;
    const void *watch=nullptr;
    unsigned watchClean=0,watchInvalidate=0;
    bool failNextInvalidate=false;
    IOReturn maintain(void *address,size_t,NativeMaliCacheOperation op) override {
        if(op==NativeMaliCacheClean) {
            ++clean;
            if(address==watch)++watchClean;
        }
        if(op==NativeMaliCacheInvalidate) {
            ++invalidate;
            if(address==watch)++watchInvalidate;
            if(failNextInvalidate) {
                failNextInvalidate=false;
                return kIOReturnIOError;
            }
        }
        return kIOReturnSuccess;
    }
};
int main() {
    RegisterFile regs;NativeMaliPower power;NativeMaliMmu mmu;NativeMaliGpuCore gpu;
    NativeMaliJobSlot jobs;Pages pages;Buffers buffers;Cache cache;
    assert(mmu.prepare(&regs,&power)==0 && mmu.setPresent(0xff)==0);
    assert(gpu.prepare(&regs,&power)==0);
    assert(jobs.prepare(&regs,&power,&mmu)==0 && jobs.setPresent(7,3)==0);
    NativeMaliGpuObservation obs{};obs.gpuId=0x74021000;obs.ias=48;obs.oas=40;
    auto a=std::make_unique<NativeMaliUserClient>();
    auto b=std::make_unique<NativeMaliUserClient>();
    NativeMaliVm va,vb;uint32_t aa=0,ab=0;
    assert(mmu.allocate(&aa)==0 && mmu.allocate(&ab)==0 && aa!=ab);
    assert(va.prepare(&pages,48,40,false,&cache)==0);
    assert(vb.prepare(&pages,48,40,false,&cache)==0);
    assert(mmu.enable(aa,va)==0 && mmu.enable(ab,vb)==0);
    assert(a->open(&buffers,&cache,false,&va,&mmu,aa,&jobs,&gpu,obs,1<<20)==0);
    assert(b->open(&buffers,&cache,false,&vb,&mmu,ab,&jobs,&gpu,obs,1<<20)==0);
    NativeMaliAbiBoAlloc req{0x10000};NativeMaliAbiBoAllocReply bo{};
    assert(a->allocBo(req,&bo)==0);
    NativeMaliAbiBoMap map{bo.handle,0x1000000,3,0};NativeMaliAbiBoMapReply mapped{};
    assert(a->mapBo(map,&mapped)==0);
    auto *memory=a->memoryForHandle(bo.handle);assert(memory && memory->mapped());
    auto *cpu=static_cast<uint8_t *>(memory->cpu());
    NativeMaliAbiBoAllocReply input{};
    assert(a->allocBo(req,&input)==0);
    assert(a->mapBo({input.handle,0x1100000,NativeMaliBoRead,0},&mapped)==0);
    cache.watch=a->memoryForHandle(input.handle)->cpu();
    NativeMaliAbiSubmit submit{0x1000000,0,0,0,0};NativeMaliAbiSubmitReply reply{};
    auto writeHeader=[&](unsigned type,uint64_t next=0) {
        NativeMaliJobHeader h;h.type=type;h.index=1;h.next=next;
        NativeMaliJobChain::packHeader(cpu,h);
    };
    writeHeader(NativeMaliJobFragment);
    assert(b->submit(submit,&reply)==kIOReturnBadArgument && regs.submitted.empty());
    assert(a->submit(submit,&reply)==0 && regs.submitted.back()==0);
    NativeMaliAbiBoFree free{bo.handle};
    assert(a->freeBo(free)==kIOReturnBusy);
    assert(a->submit(submit,&reply)==kIOReturnBusy);
    // Busy cleared the reply; the fence was the first JS0 submission.
    NativeMaliAbiWaitFence wait{NativeMaliAbiValidate::packFence(0,1),100000000};
    assert(b->waitFence(wait)==kIOReturnBadArgument);
    assert(a->waitFence(wait)==0 && cache.invalidate==1 && cache.watchClean==1 && cache.watchInvalidate==0);
    const NativeMaliAbiWaitFence first=wait;
    assert(a->waitFence(first)==0 && cache.invalidate==1);
    wait.timeout_ns=1000000001;
    assert(a->waitFence(wait)==kIOReturnBadArgument);
    writeHeader(NativeMaliJobWriteValue);
    regs.completeOnStart=false;
    assert(a->submit(submit,&reply)==0 && regs.submitted.back()==1);
    wait={reply.fence,0};
    assert(a->waitFence(wait)==kIOReturnTimeout && cache.invalidate==1);
    assert(a->waitFence(first)==0 && cache.invalidate==1 && jobs.outstanding(1));
    regs.complete(1);
    cache.failNextInvalidate=true;
    assert(a->waitFence(wait)==kIOReturnIOError && cache.invalidate==2 && !jobs.outstanding(1));
    assert(a->waitFence(first)==0 && cache.invalidate==2);
    assert(a->freeBo(free)==kIOReturnBusy && a->submit(submit,&reply)==kIOReturnBusy);
    assert(a->waitFence(wait)==0 && cache.invalidate==3);
    assert(a->waitFence(wait)==0 && cache.invalidate==3);
    regs.completeOnStart=true;
    writeHeader(NativeMaliJobCompute);
    assert(a->submit(submit,&reply)==0 && regs.submitted.back()==2);
    const uint64_t dependency=reply.fence;
    submit.wait_fence=dependency;
    // Submitting with a dependency reaps and synchronizes that fence once.
    assert(a->submit(submit,&reply)==0 && regs.submitted.back()==2 && cache.invalidate==4);
    assert(a->waitFence({dependency,0})==0 && cache.invalidate==4 && jobs.outstanding(2));
    wait={reply.fence,100000000};
    assert(a->waitFence(wait)==0 && cache.invalidate==5);
    assert(cache.watchClean==4 && cache.watchInvalidate==0);
    submit.wait_fence=0;
    const auto starts=regs.submitted.size();
    writeHeader(NativeMaliJobFragment,0x1000040);
    NativeMaliJobHeader next;next.type=NativeMaliJobCompute;next.index=2;
    NativeMaliJobChain::packHeader(cpu+64,next);
    assert(a->submit(submit,&reply)==kIOReturnBadArgument);
    writeHeader(NativeMaliJobFragment,0x1000000);
    assert(a->submit(submit,&reply)==kIOReturnBadArgument);
    writeHeader(127);
    assert(a->submit(submit,&reply)==kIOReturnUnsupported);
    submit.job_chain=0x100ffc0;writeHeader(NativeMaliJobFragment);
    assert(a->submit(submit,&reply)==kIOReturnUnsupported);
    submit.job_chain=0x1010000;
    assert(a->submit(submit,&reply)==kIOReturnBadArgument);
    assert(starts==regs.submitted.size());
    NativeMaliAbiQuery query{};query.abi_version=2;
    uint8_t output[sizeof(query)];memset(output,0x5a,sizeof(output));
    assert(a->dispatch(NativeMaliMethodQuery,&query,sizeof(query)-1,output,sizeof(output))==kIOReturnBadArgument);
    for(auto v:output)assert(v==0x5a);
    // A failed hardware completion cannot turn into success on a retry.
    submit.job_chain=0x1000000;
    writeHeader(NativeMaliJobFragment);
    assert(a->submit(submit,&reply)==0);
    regs.data[JS_STATUS(0)/4]=0x40;
    regs.data[JOB_INT_RAWSTAT/4]|=JOB_INT_MASK_ERR(0);
    wait={reply.fence,0};
    assert(a->waitFence(wait)==kIOReturnIOError && cache.invalidate==5);
    assert(a->waitFence(wait)==kIOReturnIOError && cache.invalidate==5);
    NativeMaliAbiStatus status{};
    assert(a->status(&status)==0 && status.faults==1);
    assert(a->freeBo(free)==0);
    assert(a->releaseAll()==0 && b->releaseAll()==0);
    assert(mmu.disable(aa)==0 && mmu.disable(ab)==0);
    assert(mmu.release(aa)==0 && mmu.release(ab)==0);
    va.reset();vb.reset();
    assert(buffers.live==0 && pages.live==0 && mmu.allocatedMask()==0);
    puts("PASS: per-AS routing, fence-bound DMA sync, read-only BOs, sticky faults and cleanup.");
}
