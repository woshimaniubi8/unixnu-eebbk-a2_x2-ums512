// SPDX-License-Identifier: GPL-2.0-only
// Pinned Mali-G52/Panfrost MMU + job code, adapted to this board's live maps
// and the same original IOBufferMemoryDescriptor API used by SdhciBlock.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include "mali/NativeMaliGpuCore.h"
#include "mali/NativeMaliMmu.h"
#include "mali/NativeMaliJob.h"
#include "mali/NativeMaliJobSlot.h"
#include "mali/NativeMaliRegs.h"

extern "C" void sprd_mali_trace_mark(unsigned step,const char *text){IOLog("M119 mali: reset step=%u %s\n",step,text);}
namespace {
class Registers final:public NativeMaliRegisterTarget {
    volatile uint8_t *base;
public:
    explicit Registers(volatile uint8_t *p):base(p){}
    IOReturn read32(uint32_t at,uint32_t *v) override{
        if(!v || at>=0x100000 || (at&3))return kIOReturnBadArgument;
        *v=*(volatile uint32_t *)(base+at);__asm__ volatile("dsb ld":::"memory");return kIOReturnSuccess;
    }
    IOReturn write32(uint32_t at,uint32_t v) override{
        if(at>=0x100000 || (at&3))return kIOReturnBadArgument;
        __asm__ volatile("dsb st":::"memory");*(volatile uint32_t *)(base+at)=v;
        __asm__ volatile("dsb sy":::"memory");return kIOReturnSuccess;
    }
    IOReturn delay(uint64_t us) override{
        if(us>1000000)return kIOReturnBadArgument;IODelay((unsigned)us);return kIOReturnSuccess;
    }
};
class Cache final:public NativeMaliCacheMaintainer {
public:
    IOReturn maintain(void *cpu,size_t bytes,NativeMaliCacheOperation op) override{
        if(!cpu || !bytes || (uintptr_t)cpu+bytes<(uintptr_t)cpu)return kIOReturnBadArgument;
        uint64_t ctr=0;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));
        size_t line=4u<<((ctr>>16)&15);if(line<16 || line>256)return kIOReturnUnsupported;
        uintptr_t end=(uintptr_t)cpu+bytes;
        for(uintptr_t p=(uintptr_t)cpu&~(line-1);p<end;p+=line){
            if(op==NativeMaliCacheClean)__asm__ volatile("dc cvac,%0"::"r"(p):"memory");
            else if(op==NativeMaliCacheInvalidate)__asm__ volatile("dc ivac,%0"::"r"(p):"memory");
            else if(op==NativeMaliCacheCleanInvalidate)__asm__ volatile("dc civac,%0"::"r"(p):"memory");
            else return kIOReturnBadArgument;
        }__asm__ volatile("dsb sy":::"memory");return kIOReturnSuccess;
    }
};
IOBufferMemoryDescriptor *allocate(){
    auto *b=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),0x4000,0xffffffffULL);
    if(!b)return nullptr;
    if(b->prepare()!=kIOReturnSuccess){b->release();return nullptr;}
    IOByteCount length=0;uint64_t physical=b->getPhysicalSegment(0,&length);
    if(!physical || (physical&0x3fff) || length<0x4000 || physical>0xffffc000ULL){b->complete();b->release();return nullptr;}
    return b;
}
class Pages final:public NativeMaliPageSource {
    IOBufferMemoryDescriptor *descriptors[NativeMaliVmMaxTables]={};
public:
    IOReturn alloc(NativeMaliPage *out) override{
        if(!out)return kIOReturnBadArgument;
        for(auto &d:descriptors)if(!d){
            d=allocate();if(!d)return kIOReturnNoMemory;
            IOByteCount n=0;*out={d->getBytesNoCopy(),d->getPhysicalSegment(0,&n),4096};return kIOReturnSuccess;
        }return kIOReturnNoResources;
    }
    void release(const NativeMaliPage &page) override{
        for(auto &d:descriptors){
            if(d && d->getBytesNoCopy()==page.cpu){d->complete();d->release();d=nullptr;return;}
        }
    }
};
struct Resources {
    Pages pages;Cache cache;NativeMaliVm vm;
    IOBufferMemoryDescriptor *buffer=nullptr;
    ~Resources(){vm.reset();if(buffer){buffer->complete();buffer->release();}}
};
// On failed reset/MMU quiescence these references remain pinned. No GPU-walked
// page is returned to the allocator while hardware may still access it.
Resources *quarantined=nullptr;
}
extern "C" IOReturn sprd_mali_owned_write_test(volatile uint8_t *gpu,volatile uint8_t *pmu,
    volatile uint8_t *aon,volatile uint8_t *clock,bool *canRestore){
    if(!gpu || !pmu || !aon || !clock || !canRestore)return kIOReturnBadArgument;
    *canRestore=true;if(quarantined)return kIOReturnNotReady;
    Registers regs(gpu);NativeMaliPower power(pmu,aon,clock);NativeMaliGpuCore core;
    NativeMaliMmu mmu;NativeMaliJobSlot slots;NativeMaliGpuObservation obs;
    auto *r=new Resources;if(!r)return kIOReturnNoMemory;
    unsigned stage=1;uint32_t as=0;bool asLive=false,jobLive=false;
    IOReturn result=core.prepare(&regs,&power);
    if(result==kIOReturnSuccess)result=core.probe(&obs);
    IOLog("M119 mali: features result=0x%x id=0x%x mmu=%u/%u shader=0x%llx js=0x%x\n",result,obs.gpuId,obs.ias,obs.oas,(unsigned long long)obs.shaderPresent,obs.jsPresent);
    if(result==kIOReturnSuccess && (obs.gpuId!=0x74021000 || !(obs.jsPresent&2)))result=kIOReturnUnsupported;
    if(result==kIOReturnSuccess){stage=2;result=core.softReset();regs.write32(GPU_INT_MASK,0);}
    if(result==kIOReturnSuccess){stage=3;result=core.powerOn();}
    if(result==kIOReturnSuccess){stage=4;result=mmu.prepare(&regs,&power);}
    if(result==kIOReturnSuccess)result=mmu.setPresent(obs.asPresent);
    if(result==kIOReturnSuccess)result=mmu.allocate(&as);
    if(result==kIOReturnSuccess)result=r->vm.prepare(&r->pages,obs.ias,obs.oas,false,&r->cache);
    if(result==kIOReturnSuccess){stage=5;r->buffer=allocate();if(!r->buffer)result=kIOReturnNoMemory;}
    constexpr uint64_t address=0x1000000,target=address+4096,expected=0x4750554d313139ULL;
    uint64_t got=0;NativeMaliJobCompletion completion;NativeMaliFence fence;
    if(result==kIOReturnSuccess){
        IOByteCount n=0;uint64_t physical=r->buffer->getPhysicalSegment(0,&n);
        bzero(r->buffer->getBytesNoCopy(),0x4000);
        result=r->vm.map(address,physical,8192,NativeMaliVmRead|NativeMaliVmWrite|NativeMaliVmCached);
        IOLog("M119 mali: owned buffer physical=0x%llx iova=0x%llx tables=%llu bytes=0x2000\n",(unsigned long long)physical,(unsigned long long)address,(unsigned long long)r->vm.tables());
    }
    if(result==kIOReturnSuccess){stage=6;asLive=true;result=mmu.enable(as,r->vm);}
    NativeMaliJobChain chain;
    if(result==kIOReturnSuccess)result=chain.prepare(r->buffer->getBytesNoCopy(),address,4096);
    uint32_t index=0;
    if(result==kIOReturnSuccess)result=chain.addWriteValue(NativeMaliWriteValueImmediate64,target,expected,&index);
    if(result==kIOReturnSuccess)result=r->cache.maintain(r->buffer->getBytesNoCopy(),0x4000,NativeMaliCacheCleanInvalidate);
    if(result==kIOReturnSuccess)result=slots.prepare(&regs,&power,&mmu);
    if(result==kIOReturnSuccess)result=slots.setPresent(obs.jsPresent,obs.shaderPresent);
    uint32_t flush=0;
    if(result==kIOReturnSuccess)result=core.latestFlushId(&flush);
    if(result==kIOReturnSuccess){stage=7;jobLive=true;result=slots.submit(1,chain.firstJob(),as,flush,&fence);}
    if(result==kIOReturnSuccess){stage=8;result=slots.waitForCompletion(1,&completion,100000,10);}
    if(result==kIOReturnSuccess){
        result=r->cache.maintain(r->buffer->getBytesNoCopy(),0x4000,NativeMaliCacheInvalidate);
        if(result==kIOReturnSuccess){got=*(volatile uint64_t *)((uint8_t *)r->buffer->getBytesNoCopy()+4096);if(got!=expected)result=kIOReturnIOError;}
    }
    NativeMaliMmuFault fault{};if(asLive)mmu.readFault(as,&fault);
    IOLog("M119 mali: WRITE_VALUE stage=%u result=0x%x raw=0x%x js=0x%x expected=0x%llx actual=0x%llx as_fault=0x%x va=0x%x%08x\n",stage,result,completion.rawStatus,completion.jsStatus,(unsigned long long)expected,(unsigned long long)got,fault.status,fault.addressHi,fault.addressLo);
    IOReturn stopped=jobLive?slots.quiesce():kIOReturnSuccess;
    IOReturn disabled=asLive?mmu.disable(as):kIOReturnSuccess;
    IOReturn off=core.prepared()?core.powerOff():kIOReturnSuccess;
    IOReturn reset=core.prepared()?core.softReset():kIOReturnSuccess;
    regs.write32(GPU_INT_MASK,0);regs.write32(JOB_INT_MASK,0);regs.write32(MMU_INT_MASK,0);
    bool released=(stopped==kIOReturnSuccess && disabled==kIOReturnSuccess) || reset==kIOReturnSuccess;
    IOLog("M119 mali: cleanup stop=0x%x disable=0x%x off=0x%x reset=0x%x memory_released=%u\n",stopped,disabled,off,reset,released);
    if(released)delete r;else {quarantined=r;*canRestore=false;}
    if(result==kIOReturnSuccess && (!released || off!=kIOReturnSuccess || reset!=kIOReturnSuccess))result=kIOReturnIOError;
    return result;
}
