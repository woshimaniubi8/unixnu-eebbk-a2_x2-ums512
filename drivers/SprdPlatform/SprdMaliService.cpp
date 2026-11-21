// SPDX-License-Identifier: GPL-2.0-only
// Persistent board-owned Mali service and upstream NativeMali user-client ABI.
// Its IOAcceleratorES metaclass is registered after the original IOGPUFamily.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include "mali/NativeMaliGpuCore.h"
#include "mali/NativeMaliJobSlot.h"
#include "mali/NativeMaliMmu.h"
#include "mali/NativeMaliRegs.h"
#include "SprdMaliSession.h"
#include "SprdMaliRender.h"
#include "SprdMaliFullFrame.h"
#include "SprdMaliCompute.h"
#include "SprdMaliPresent.h"
#include "mali/OriginalAcceleratorABI.h"
#include "mali/NativeMaliSystemPolicy.h"
#include <kern/task.h>
#include <mach/kmod.h>
#include <mach/vm_param.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <arm/pmap_public.h>

// The component SDK exports the public PA type, not arm/pmap/pmap.h.
// Match XNU 10063.141.1's C definition without importing its private structs.
extern "C" boolean_t pmap_valid_address(pmap_paddr_t address);
extern "C" void flush_dcache64(addr64_t address,unsigned bytes,int physical);
extern "C" void proc_selfname(char *buffer,int size);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void sprd_mali_service_meta_init() __asm__("__GLOBAL__sub_I_SprdMaliService.cpp");

namespace {

class Registers final : public NativeMaliRegisterTarget {
    volatile uint8_t *base_ = nullptr;
    size_t bytes_ = 0;
public:
    void bind(volatile uint8_t *base, size_t bytes) { base_ = base; bytes_ = bytes; }
    bool bound() const { return base_ && bytes_; }
    IOReturn read32(uint32_t offset, uint32_t *value) override
    {
        if(!value || !base_ || (offset & 3u) || offset > bytes_ || bytes_ - offset < 4)
            return kIOReturnBadArgument;
        *value = *reinterpret_cast<volatile uint32_t *>(base_ + offset);
        __asm__ volatile("dsb ld" ::: "memory");
        return kIOReturnSuccess;
    }
    IOReturn write32(uint32_t offset, uint32_t value) override
    {
        if(!base_ || (offset & 3u) || offset > bytes_ || bytes_ - offset < 4)
            return kIOReturnBadArgument;
        *reinterpret_cast<volatile uint32_t *>(base_ + offset) = value;
        __asm__ volatile("dsb sy" ::: "memory");
        return kIOReturnSuccess;
    }
    IOReturn delay(uint64_t microseconds) override
    {
        if(microseconds > UINT32_MAX)return kIOReturnBadArgument;
        IODelay(static_cast<UInt32>(microseconds));
        return kIOReturnSuccess;
    }
};

struct Window {
    uint64_t base;
    size_t bytes;
    IOMemoryMap *map = nullptr;
    Registers regs;
};

struct Presenter {
    NativeMaliXnuCache cache;
    NativeMaliXnuPages pages;
    NativeMaliXnuBuffers buffers;
    NativeMaliVm vm;
    NativeMaliMemory packet;
    NativeMaliVmSpan spans[NativeMaliMemoryMaxSpans]{};
    IOMemoryDescriptor *source=nullptr;
    uint64_t frames=0;
    uint32_t as=0;
    bool allocated=false,enabled=false,ready=false,verify=false,sourceMapped=false;
    NativeMaliFence pending{};
};

} // namespace

class NativeSprdMaliService final : public IOAcceleratorES {
    OSDeclareDefaultStructors(NativeSprdMaliService)
    Window windows_[4]{};
    NativeMaliPower *power_ = nullptr;
    NativeMaliGpuCore core_;
    NativeMaliMmu mmu_;
    NativeMaliJobSlot jobs_;
    NativeMaliGpuObservation observation_{};
    bool factorySupplyQualified_ = false;
    uint32_t factorySupplyMicrovolts_ = 0;
    bool powerAcquired_ = false;
    uint32_t savedGpuInterruptMask_ = 0;
    bool gpuInterruptMaskSaved_ = false;
    bool corePrepared_ = false;
    bool resetDone_ = false;
    bool powered_ = false;
    bool mmuPrepared_ = false;
    bool jobsPrepared_ = false;
    bool ready_ = false;
    IOLock *sessionLock_=nullptr;
    uint32_t sessions_=0;
    SprdMaliSession *quarantined_[NativeMaliMmuMaxAs]={};
    Presenter *presenter_=nullptr;
    bool presenterFailed_=false;
    IOReturn sharedMemoryError_=kIOReturnSuccess;

    IOReturn setupPresenter(bool verify);
    IOReturn closePresenter();

    void describeWindows()
    {
        windows_[0].base = 0x60000000;
        windows_[0].bytes = 0x100000; /* Mali core/JM/MMU */
        windows_[1].base = 0x327e0000;
        windows_[1].bytes = 0x1000;   /* GPU PMU gate */
        windows_[2].base = 0x327d0000;
        windows_[2].bytes = 0x3000;   /* AON APB gate */
        windows_[3].base = 0x60100000;
        windows_[3].bytes = 0x3000;   /* GPU clock */
    }

    void releaseWindows()
    {
        for(auto &window : windows_) {
            window.regs.bind(nullptr, 0);
            if(window.map) {
                window.map->release();
                window.map = nullptr;
            }
        }
    }

    void stopHardware()
    {
        if(jobsPrepared_) {
            (void)jobs_.quiesce();
            jobsPrepared_ = false;
        }
        if(mmuPrepared_) {
            mmu_.resetAllocation();
            mmuPrepared_ = false;
        }
        if(corePrepared_ && powered_) {
            (void)core_.powerOff();
            powered_ = false;
        }
        if(corePrepared_ && resetDone_) {
            (void)core_.softReset();
            resetDone_ = false;
        }
        if(gpuInterruptMaskSaved_) {
            (void)windows_[0].regs.write32(GPU_INT_MASK, savedGpuInterruptMask_);
            gpuInterruptMaskSaved_ = false;
        }
        if(power_) {
            if(powerAcquired_) {
                const IOReturn released = power_->release();
                IOLog("M150 mali: power release=0x%x\n", released);
                powerAcquired_ = false;
            }
            delete power_;
            power_ = nullptr;
        }
        ready_ = false;
        corePrepared_ = false;
    }

public:
    IOReturn sharedMemoryError() const { return sharedMemoryError_; }
    void failSharedMemory(IOReturn error) {
        if(sharedMemoryError_==kIOReturnSuccess)sharedMemoryError_=error;
    }
    void setFactorySupplyMicrovolts(uint32_t microvolts) {
        factorySupplyMicrovolts_=microvolts;
        factorySupplyQualified_=microvolts>=700000;
    }
    bool start(IOService *provider) override
    {
        if(!IOService::start(provider))return false;
        sessionLock_=IOLockAlloc();
        if(!sessionLock_)return false;
        describeWindows();
        const IOReturn mapping = [&]() {
            for(auto &window : windows_) {
                IODeviceMemory *memory = IODeviceMemory::withRange(
                    window.base, window.bytes);
                if(!memory)return kIOReturnNoMemory;
                window.map = memory->map(kIOMapInhibitCache);
                memory->release();
                if(!window.map || !window.map->getVirtualAddress())
                    return kIOReturnNoMemory;
                window.regs.bind(reinterpret_cast<volatile uint8_t *>(
                    window.map->getVirtualAddress()), window.bytes);
            }
            return kIOReturnSuccess;
        }();
        if(mapping != kIOReturnSuccess) {
            IOLog("M150 mali: service map failed=0x%x\n", mapping);
            stop(provider);
            return false;
        }

        power_ = new NativeMaliPower(
            reinterpret_cast<volatile uint8_t *>(windows_[1].map->getVirtualAddress()),
            reinterpret_cast<volatile uint8_t *>(windows_[2].map->getVirtualAddress()),
            reinterpret_cast<volatile uint8_t *>(windows_[3].map->getVirtualAddress()));
        if(!power_) {
            IOLog("M150 mali: service power allocation failed\n");
            stop(provider);
            return false;
        }
        uint32_t coreMHz=26;
        PE_parse_boot_argn("bbk-mali-clock",&coreMHz,sizeof(coreMHz));
        SprdMali::ClockProfile profile;
        const bool knownProfile=SprdMali::clockProfile(coreMHz,profile);
        const bool voltageQualified=knownProfile && factorySupplyQualified_ &&
            SprdMali::profileSupplyReady(profile,factorySupplyMicrovolts_);
        IOReturn result = knownProfile?power_->acquire(voltageQualified,coreMHz):kIOReturnBadArgument;
        powerAcquired_ = result == kIOReturnSuccess;
        if(result==kIOReturnSuccess) {
            IOLog("M165 mali: factory clock core=%u mem=%u sys=%u MHz voltage_qualified=%u supply_uv=%u minimum_uv=%u\n",
                profile.coreMHz,profile.memoryMHz,profile.systemMHz,voltageQualified?1u:0u,
                factorySupplyMicrovolts_,profile.minimumMicrovolts);
        }
        IOLog("M150 mali: power acquire=0x%x factory_supply=%u pmu=0x%x aon=0x%x clocks=0x%x/0x%x/0x%x\n",
            result, factorySupplyQualified_ ? 1u : 0u,
            *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<volatile uint8_t *>(windows_[1].map->getVirtualAddress()) + 0x30),
            *reinterpret_cast<volatile uint32_t *>(windows_[2].map->getVirtualAddress()),
            *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<volatile uint8_t *>(windows_[3].map->getVirtualAddress()) + 4),
            *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<volatile uint8_t *>(windows_[3].map->getVirtualAddress()) + 8),
            *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<volatile uint8_t *>(windows_[3].map->getVirtualAddress()) + 0xc));
        if(result == kIOReturnSuccess)IODelay(700);
        if(result == kIOReturnSuccess)result = windows_[0].regs.read32(GPU_INT_MASK, &savedGpuInterruptMask_);
        gpuInterruptMaskSaved_ = result == kIOReturnSuccess;
        if(result == kIOReturnSuccess)result = core_.prepare(&windows_[0].regs, power_);
        corePrepared_ = result == kIOReturnSuccess;
        if(result == kIOReturnSuccess)result = core_.probe(&observation_);
        IOLog("M150 mali: service probe result=0x%x id=0x%x js=0x%x as=0x%x shader=0x%llx\n",
            result, observation_.gpuId, observation_.jsPresent, observation_.asPresent,
            static_cast<unsigned long long>(observation_.shaderPresent));
        if(result == kIOReturnSuccess && observation_.gpuId != 0x74021000)
            result = kIOReturnUnsupported;
        if(result == kIOReturnSuccess) {
            result = core_.softReset();
            resetDone_ = result == kIOReturnSuccess;
        }
        if(result == kIOReturnSuccess) {
            result = core_.powerOn();
            powered_ = result == kIOReturnSuccess;
        }
        if(result == kIOReturnSuccess) {
            result = mmu_.prepare(&windows_[0].regs, power_);
            mmuPrepared_ = result == kIOReturnSuccess;
        }
        if(result == kIOReturnSuccess)result = mmu_.setPresent(observation_.asPresent);
        if(result == kIOReturnSuccess) {
            NativeMaliJobSlot *jobs = &jobs_;
            result = jobs->prepare(&windows_[0].regs, power_, &mmu_);
            jobsPrepared_ = result == kIOReturnSuccess;
            if(result == kIOReturnSuccess)
                result = jobs->setPresent(observation_.jsPresent,
                    observation_.shaderPresent);
        }
        if(result != kIOReturnSuccess) {
            IOLog("M150 mali: service start failed=0x%x\n", result);
            stop(provider);
            return false;
        }
        setProperty("gpu-id", observation_.gpuId, 32);
        setProperty("job-slots", observation_.jsPresent, 32);
        setProperty("service-ready", 1, 32);
        setName("NativeMaliGpuService");
        setProperty("model","Mali-G52");
        setProperty("MetalPluginName","VivaMTLDriver");
        setProperty("MetalPluginClassName","VMDevice");
        setProperty("NativeSandboxGrantProcess",NativeMaliSystemPolicy::GrantProcessList);
        ready_ = true;
        setProperty("NativeMaliABI",NativeMaliAbiVersion,32);
        IOLog("M150 mali: service published ready=1 id=0x%x js=0x%x\n",
            observation_.gpuId, observation_.jsPresent);
        const IOReturn test=exerciseSession();
        setProperty("gpu-session-test",static_cast<unsigned long long>(static_cast<uint32_t>(test)),32);
        IOLog("M153 mali: userclient ready abi=%u session_test=0x%x cpu_page=16384 gpu_page=4096\n",
            NativeMaliAbiVersion,test);
        if(test!=kIOReturnSuccess) {
            stop(provider);
            return false;
        }
        uint32_t fullFrame=0;
        if(PE_parse_boot_argn("bbk-mali-fullframe",&fullFrame,sizeof(fullFrame)) && fullFrame==1) {
            const IOReturn frameTest=exerciseFullFrame();
            setProperty("gpu-full-frame-test",static_cast<uint32_t>(frameTest),32);
            if(frameTest!=kIOReturnSuccess) {
                ready_=false;
                setProperty("service-ready",static_cast<unsigned long long>(0),32);
                // Do not publish a failed backend, or drop an unconfirmed DMA
                // mapping. Keep the attached service alive for platform stop.
                if(!sessions_)stopHardware();
                IOLog("M161 mali: service not published after full-frame test\n");
                return true;
            }
        }
        uint32_t compute=0;
        if(PE_parse_boot_argn("bbk-mali-compute",&compute,sizeof(compute)) && compute==1) {
            const IOReturn result=exerciseCompute();
            setProperty("gpu-compute-test",static_cast<uint32_t>(result),32);
            if(result!=kIOReturnSuccess) {
                ready_=false;
                setProperty("service-ready",static_cast<unsigned long long>(0),32);
                if(!sessions_)stopHardware();
                IOLog("M163 mali: service not published after compute test\n");
                return true;
            }
        }
        uint32_t present=0;
        if(PE_parse_boot_argn("bbk-mali-present",&present,sizeof(present)) && present==1) {
            uint32_t verify=0;
            PE_parse_boot_argn("bbk-mali-present-verify",&verify,sizeof(verify));
            const IOReturn status=setupPresenter(verify==1);
            presenterFailed_=status!=kIOReturnSuccess;
            IOLog("M164 mali: display setup status=0x%x verify=%u\n",status,verify==1?1u:0u);
        }
        registerService();
        return true;
    }

    IOLock *sessionLock() const { return sessionLock_; }
    bool ready() const { return ready_; }
    IOReturn openSession(SprdMaliSession *session);
    IOReturn closeSession(SprdMaliSession *session);
    void quarantine(SprdMaliSession *session) {
        ready_=false;
        setProperty("service-ready",static_cast<unsigned long long>(0),32);
        if(session && session->as<NativeMaliMmuMaxAs)quarantined_[session->as]=session;
    }
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
    IOReturn exerciseSession();
    IOReturn exerciseFullFrame();
    IOReturn exerciseCompute();
    IOReturn presentFrame(IOMemoryDescriptor *,IOMemoryMap *);

    void free() override {
        if(sessionLock_ && !sessions_ && !presenter_) { IOLockFree(sessionLock_); sessionLock_=nullptr; }
        IOService::free();
    }

    void stop(IOService *provider) override
    {
        IOLog("M150 mali: service stop ready=%u\n", ready_ ? 1u : 0u);
        if(sessionLock_)IOLockLock(sessionLock_);
        ready_=false;
        const IOReturn displayClosed=closePresenter();
        if(sessionLock_)IOLockUnlock(sessionLock_);
        if(displayClosed!=kIOReturnSuccess) {
            IOLog("M164 mali: retained display DMA resources status=0x%x\n",displayClosed);
            return;
        }
        if(sessions_) {
            IOLog("M153 mali: stop retains %u session(s) and hardware ownership\n",sessions_);
            return;
        }
        stopHardware();
        releaseWindows();
        IOService::stop(provider);
    }
};

OSDefineMetaClassAndStructors(NativeSprdMaliService, IOAcceleratorES)



IOReturn NativeSprdMaliService::openSession(SprdMaliSession *s)
{
    if(!s || !ready_ || s->allocated)return kIOReturnNotReady;
    s->pages.setOutputLimit(observation_.oas);
    s->buffers.setOutputLimit(observation_.oas);
    IOReturn result=mmu_.allocate(&s->as);
    if(result!=kIOReturnSuccess)return result;
    s->allocated=true;
    ++sessions_;
    result=s->vm.prepare(&s->pages,observation_.ias,observation_.oas,false,&s->cache);
    if(result==kIOReturnSuccess) {
        // Preserve all table pages until disable has proved that AS is inactive.
        s->enabled=true;
        result=mmu_.enable(s->as,s->vm);
    }
    if(result==kIOReturnSuccess) {
        result=s->core.open(&s->buffers,&s->cache,false,&s->vm,&mmu_,s->as,
            &jobs_,&core_,observation_,NATIVE_MALI_ABI_MAX_BO_BYTES);
        s->opened=result==kIOReturnSuccess;
    }
    IOLog("M153 mali: session open as=%u result=0x%x sessions=%u\n",s->as,result,sessions_);
    return result;
}

IOReturn NativeSprdMaliService::closeSession(SprdMaliSession *s)
{
    if(!s)return kIOReturnBadArgument;
    IOReturn result=kIOReturnSuccess;
    if(s->opened) {
        result=s->core.releaseAll();
        if(result!=kIOReturnSuccess)return result;
        s->opened=false;
    }
    if(s->enabled) {
        result=mmu_.disable(s->as);
        if(result!=kIOReturnSuccess)return result;
        s->enabled=false;
    }
    if(s->allocated) {
        result=mmu_.release(s->as);
        if(result!=kIOReturnSuccess)return result;
        s->allocated=false;
        --sessions_;
    }
    s->vm.reset();
    IOLog("M153 mali: session close as=%u result=0x%x sessions=%u\n",s->as,result,sessions_);
    return result;
}

OSDefineMetaClassAndStructors(NativeSprdMaliUserClient,IOUserClient)

bool NativeSprdMaliUserClient::initWithTask(task_t task,void *security,UInt32 type,
    OSDictionary *properties)
{
    if(type!=0 || task==TASK_NULL ||
        !IOUserClient::initWithTask(task,security,type,properties))return false;
    owningTask_=task;return true;
}

bool NativeSprdMaliUserClient::start(IOService *provider)
{
    if(!IOUserClient::start(provider))return false;
    owner_=OSDynamicCast(NativeSprdMaliService,provider);
    if(!owner_ || !owner_->sessionLock())return false;
    session_=new SprdMaliSession;
    if(!session_)return false;
    IOLockLock(owner_->sessionLock());
    IOReturn result=session_->buffers.setTask(owningTask_);
    if(result==kIOReturnSuccess)result=owner_->openSession(session_);
    if(result!=kIOReturnSuccess) {
        const IOReturn closed=owner_->closeSession(session_);
        if(closed==kIOReturnSuccess)delete session_;
        else owner_->quarantine(session_);
        session_=nullptr;
    }
    IOLockUnlock(owner_->sessionLock());
    return result==kIOReturnSuccess;
}

void NativeSprdMaliUserClient::releaseViews()
{
    for(auto &view:views_) {
        if(view)view->release();
        view=nullptr;
    }
}

IOReturn NativeSprdMaliUserClient::shutdown()
{
    if(!owner_ || !session_)return kIOReturnSuccess;
    IOLockLock(owner_->sessionLock());
    const IOReturn result=owner_->closeSession(session_);
    if(result==kIOReturnSuccess) {
        releaseViews();
        delete session_;
        session_=nullptr;
    }
    IOLockUnlock(owner_->sessionLock());
    return result;
}

void NativeSprdMaliUserClient::stop(IOService *provider)
{
    const IOReturn result=shutdown();
    if(result!=kIOReturnSuccess && session_) {
        IOLockLock(owner_->sessionLock());
        owner_->quarantine(session_);
        session_=nullptr;
        IOLockUnlock(owner_->sessionLock());
        IOLog("M153 mali: stop quarantined AS after error=0x%x\n",result);
    }
    releaseViews();
    IOUserClient::stop(provider);
}

void NativeSprdMaliUserClient::free()
{
    const IOReturn result=shutdown();
    if(result!=kIOReturnSuccess && owner_ && session_) {
        IOLockLock(owner_->sessionLock());
        owner_->quarantine(session_);
        session_=nullptr;
        IOLockUnlock(owner_->sessionLock());
    }
    releaseViews();
    IOUserClient::free();
}

IOReturn NativeSprdMaliUserClient::clientClose()
{
    const IOReturn result=shutdown();
    if(result==kIOReturnSuccess)terminate();
    return result;
}

IOReturn NativeSprdMaliService::newUserClient(task_t task,void *security,UInt32 type,
    OSDictionary *properties,IOUserClient **handler)
{
    if(!handler)return kIOReturnBadArgument;
    *handler=nullptr;
    if(type!=0)return kIOReturnUnsupported;
    char processName[17]={};
    proc_selfname(processName,sizeof(processName));
    const bool systemMetalClient=NativeMaliSystemPolicy::allowsProcess(processName);
    if(task!=kernel_task && !systemMetalClient && IOUserClient::clientHasPrivilege(
        task,kIOClientPrivilegeAdministrator)!=kIOReturnSuccess) {
        IOLog("M194 mali: userclient denied name=%s type=%u\n",processName,type);
        return kIOReturnNotPrivileged;
    }
    if(!ready_)return kIOReturnNotReady;
    auto *client=new NativeSprdMaliUserClient;
    if(!client)return kIOReturnNoMemory;
    if(!client->initWithTask(task,security,type,properties)) {
        client->release();
        return kIOReturnNoMemory;
    }
    if(!client->attach(this)) {client->release();return kIOReturnNoResources;}
    if(!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnNotReady;
    }
    *handler=client;
    IOLog("M194 mali: client attached name=%s type=%u kernel=%u system=%u\n",
        processName,type,task==kernel_task,systemMetalClient?1u:0u);
    return kIOReturnSuccess;
}

IOReturn NativeSprdMaliUserClient::clientMemoryForType(UInt32 token,
    IOOptionBits *options,IOMemoryDescriptor **output)
{
    if(!options || !output)return kIOReturnBadArgument;
    *output=nullptr;
    if(!owner_ || !owner_->sessionLock())return kIOReturnNotReady;
    IOLockLock(owner_->sessionLock());
    IOReturn result=kIOReturnBadArgument;
    if(session_ && token && token<=NativeMaliBoTableCapacity) {
        auto *memory=session_->core.memoryForToken(token);
        if(memory) {
            auto *&view=views_[token-1];
            if(!view)view=session_->buffers.descriptor(memory->cpu());
            if(view) {
                view->retain();
                *output=view;
                *options=kIOMapAnywhere|kIOMapCopybackCache;
                result=kIOReturnSuccess;
            }
        }
    }
    IOLockUnlock(owner_->sessionLock());
    return result;
}

IOReturn NativeSprdMaliUserClient::action(OSObject *target,void *,
    IOExternalMethodArguments *args)
{
    auto *client=OSDynamicCast(NativeSprdMaliUserClient,target);
    if(!client || !args || !client->owner_)return kIOReturnBadArgument;
    auto *owner=client->owner_;
    if(args->selector==NativeMaliMethodBoWaitShared) {
        if(!IOLockTryLock(owner->sessionLock()))return kIOReturnBusy;
    } else IOLockLock(owner->sessionLock());
    IOReturn result=kIOReturnNotReady;
    auto *s=client->session_;
    if(s && owner->ready()) {
        const bool sharedSubmit=args->selector==NativeMaliMethodSubmit && s->core.hasImported();
        const bool sharedOperation=sharedSubmit || args->selector==NativeMaliMethodBoWaitShared ||
            args->selector==NativeMaliMethodBoImportUser;
        if(sharedOperation && owner->sharedMemoryError()!=kIOReturnSuccess)
            result=owner->sharedMemoryError();
        else if(!sharedSubmit)result=s->core.dispatch(args->selector,args->structureInput,
            args->structureInputSize,args->structureOutput,args->structureOutputSize);
        // Imported data may alias another task's IOSurface. Keep the device
        // lock until real completion and cache invalidation. Thus method15 is
        // a device-wide shared-memory barrier, not a synthetic fence success.
        if(sharedSubmit && owner->sharedMemoryError()==kIOReturnSuccess) {
            NativeMaliAbiSubmit request{};
            memcpy(&request,args->structureInput,sizeof(request));
            NativeMaliAbiSubmitReply reply{};
            result=s->core.submit(request,&reply);
            if(result==kIOReturnSuccess)
                result=s->core.waitFence({reply.fence,UINT64_C(1000000000)});
            if(result==kIOReturnSuccess)memcpy(args->structureOutput,&reply,sizeof(reply));
            static uint32_t reports;
            if(reports++<16 || result!=kIOReturnSuccess)
                IOLog("M188 mali: shared completion as=%u fence=0x%llx result=0x%x\n",
                    s->as,reply.fence,result);
        }
        if(sharedSubmit && (result==kIOReturnIOError || result==kIOReturnTimeout ||
            result==kIOReturnNotReady))owner->failSharedMemory(result);
        if(result==kIOReturnSuccess && args->selector==NativeMaliMethodBoImportUser)
            IOLog("M188 mali: task pages imported as=%u, forced NOEXEC\n",s->as);
        if(result==kIOReturnSuccess && args->selector==NativeMaliMethodBoCpuUnmap) {
            NativeMaliAbiBoCpuUnmap request{};
            memcpy(&request,args->structureInput,sizeof(request));
            if(request.token && request.token<=NativeMaliBoTableCapacity) {
                auto *&view=client->views_[request.token-1];
                if(view)view->release();
                view=nullptr;
            }
        }
    }
    IOLockUnlock(owner->sessionLock());
    return result;
}

IOReturn NativeSprdMaliUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *args,IOExternalMethodDispatch *,OSObject *,void *)
{
    if(!args)return kIOReturnBadArgument;
    // Reset requires a device-wide recovery implementation; it stays unsupported.
    if(selector==NativeMaliMethodReset)return kIOReturnUnsupported;
    const size_t input=NativeMaliUserClient::inputSize(selector);
    const size_t output=NativeMaliUserClient::outputSize(selector);
    if(input==NativeMaliUserClient::kUnsupportedSize ||
        output==NativeMaliUserClient::kUnsupportedSize)return kIOReturnUnsupported;
    if(args->structureInputDescriptor || args->structureOutputDescriptor)
        return kIOReturnBadArgument;
    IOExternalMethodDispatch dispatch{};
    dispatch.function=&NativeSprdMaliUserClient::action;
    dispatch.checkStructureInputSize=static_cast<uint32_t>(input);
    dispatch.checkStructureOutputSize=selector==NativeMaliMethodGetDeviceInfo
        ? kIOUCVariableStructureSize : static_cast<uint32_t>(output);
    return IOUserClient::externalMethod(selector,args,&dispatch,this,nullptr);
}

namespace {
IOReturn sessionCall(IOUserClient *client,uint32_t selector,const void *input,
    size_t inputBytes,void *output,size_t outputBytes)
{
    IOExternalMethodArguments args{};
    args.selector=selector;
    args.structureInput=input;
    args.structureInputSize=inputBytes;
    args.structureOutput=output;
    args.structureOutputSize=outputBytes;
    return client->externalMethod(selector,&args,nullptr,nullptr,nullptr);
}

uint64_t gpuElapsedUs(uint64_t start)
{
    uint64_t ns=0;
    absolutetime_to_nanoseconds(mach_absolute_time()-start,&ns);
    return ns/1000;
}
}

IOReturn NativeSprdMaliService::setupPresenter(bool verify)
{
    namespace Frame=SprdMaliPresent;
    if(presenter_ || !(observation_.jsPresent&(1u<<2)))return kIOReturnNotReady;
    auto *p=new Presenter;
    if(!p)return kIOReturnNoMemory;
    presenter_=p;
    p->verify=verify;
    p->pages.setOutputLimit(observation_.oas);
    p->buffers.setOutputLimit(observation_.oas);
    IOReturn status=mmu_.allocate(&p->as);
    if(status==kIOReturnSuccess) {
        p->allocated=true;
        status=p->vm.prepare(&p->pages,observation_.ias,observation_.oas,false,&p->cache);
    }
    if(status==kIOReturnSuccess)status=p->packet.prepare(&p->buffers,Frame::JobBytes,false,&p->cache);
    if(status==kIOReturnSuccess) {
        Frame::initialize(p->packet.cpu());
        status=p->vm.map(Frame::DestinationIova,Frame::DestinationPhysical,Frame::DestinationBytes,
            NativeMaliVmWrite|NativeMaliVmCached|NativeMaliVmNoExec);
    }
    if(status==kIOReturnSuccess) {
        // Even a partially programmed address space must be disabled before releasing pages.
        p->enabled=true;
        status=mmu_.enable(p->as,p->vm);
    }
    if(status==kIOReturnSuccess)
        status=p->packet.map(&p->vm,&mmu_,p->as,Frame::JobIova);
    if(status==kIOReturnSuccess) {
        p->ready=true;
        return status;
    }
    const IOReturn cleanup=closePresenter();
    if(cleanup!=kIOReturnSuccess)
        IOLog("M164 mali: setup cleanup status=0x%x\n",cleanup);
    return status;
}

IOReturn NativeSprdMaliService::closePresenter()
{
    auto *p=presenter_;
    if(!p)return kIOReturnSuccess;
    p->ready=false;
    if(p->pending.valid()) {
        const IOReturn status=jobs_.abort(p->pending);
        if(status!=kIOReturnSuccess)return status;
        p->pending={};
    }
    if(p->sourceMapped) {
        const IOReturn status=p->vm.unmap(SprdMaliPresent::SourceIova,
            SprdMaliPresent::SourceBytes);
        if(status!=kIOReturnSuccess)return status;
        const IOReturn flushed=mmu_.flushRange(p->as,SprdMaliPresent::SourceIova,
            SprdMaliPresent::SourceBytes);
        if(flushed!=kIOReturnSuccess)return flushed;
        p->sourceMapped=false;
    }
    if(p->packet.mapped()) {
        const IOReturn status=p->packet.unmap();
        if(status!=kIOReturnSuccess)return status;
    }
    if(p->enabled) {
        const IOReturn status=mmu_.disable(p->as);
        if(status!=kIOReturnSuccess)return status;
        p->enabled=false;
    }
    if(p->packet.prepared()) {
        const IOReturn status=p->packet.release();
        if(status!=kIOReturnSuccess)return status;
    }
    p->vm.reset();
    if(p->allocated) {
        const IOReturn status=mmu_.release(p->as);
        if(status!=kIOReturnSuccess)return status;
        p->allocated=false;
    }
    if(p->source) {
        const IOReturn status=p->source->complete(kIODirectionOut);
        if(status!=kIOReturnSuccess)return status;
        p->source->release();
        p->source=nullptr;
    }
    presenter_=nullptr;
    delete p;
    return kIOReturnSuccess;
}

IOReturn NativeSprdMaliService::presentFrame(IOMemoryDescriptor *source,IOMemoryMap *scanout)
{
    namespace Frame=SprdMaliPresent;
    if(!sessionLock_)return kIOReturnNotReady;
    IOLockLock(sessionLock_);
    auto *p=presenter_;
    const IOReturn ret=[&]() -> IOReturn {
        if(presenterFailed_)return kIOReturnUnsupported;
        if(p && !p->ready) {
            const IOReturn cleanup=closePresenter();
            if(cleanup==kIOReturnSuccess) {
                presenterFailed_=true;
                return kIOReturnUnsupported;
            }
            return cleanup;
        }
        if(!ready_ || !p || !p->ready || isInactive())return kIOReturnNotReady;
        if(jobs_.outstanding(2) || jobs_.faulted(2))return kIOReturnBusy;
        const uint64_t start=mach_absolute_time();
        const char *stage="wire";
        uint64_t setupUs=0,waitUs=0,verifyUs=0;
        size_t segments=0;
        uint32_t mismatch=0;
        const bool check=p->verify && p->frames<2;
        IOMemoryMap *input=nullptr;
        NativeMaliFence fence{};
        NativeMaliJobCompletion completion{};
        IOReturn status=[&]() -> IOReturn {
            IOReturn rc=source->prepare(kIODirectionOut);
            if(rc!=kIOReturnSuccess)return rc;
            source->retain();
            p->source=source;
            stage="segments";
            for(uint64_t offset=0;offset<Frame::SourceBytes;) {
                IOByteCount length=0;
                const addr64_t pa=source->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
                if(length>Frame::SourceBytes-offset)length=Frame::SourceBytes-offset;
                if(!Frame::sourceSpan(pa,length) || segments==NativeMaliMemoryMaxSpans)
                    return kIOReturnBadArgument;
                for(uint64_t i=0;i<length;i+=NativeMaliVmGranule)
                    if(!pmap_valid_address(pa+i))return kIOReturnBadArgument;
                p->spans[segments++]={offset,pa,length};
                offset+=length;
            }
            stage="input-cache";
            input=source->map(kIOMapAnywhere|kIOMapReadOnly|kIOMapCopybackCache);
            if(!input || !input->getVirtualAddress() || input->getLength()<Frame::SourceBytes)
                return kIOReturnNoMemory;
            // Imported mappings can still fault in. Unlike the owned packet
            // pages, do not touch them inside NativeMaliXnuCache's preemption-
            // disabled loop. Reuse the proven GOP path's XNU cache operation.
            flush_dcache64(input->getVirtualAddress(),static_cast<unsigned>(Frame::SourceBytes),false);
            stage="map";
            rc=p->vm.mapSpans(Frame::SourceIova,Frame::SourceBytes,p->spans,segments,
                NativeMaliVmRead|NativeMaliVmCached|NativeMaliVmNoExec);
            if(rc!=kIOReturnSuccess)return rc;
            p->sourceMapped=true;
            rc=mmu_.flushRange(p->as,Frame::SourceIova,Frame::SourceBytes);
            if(rc!=kIOReturnSuccess)return rc;
            setupUs=gpuElapsedUs(start);

            stage="submit";
            memset(p->packet.cpu(),0,16);
            rc=p->packet.cleanForDevice(0,Frame::JobBytes);
            uint32_t flushId=0;
            if(rc==kIOReturnSuccess)rc=core_.latestFlushId(&flushId);
            const uint64_t jobStart=mach_absolute_time();
            if(rc==kIOReturnSuccess)rc=jobs_.submit(2,Frame::JobIova,p->as,flushId,&fence);
            if(rc!=kIOReturnSuccess)return rc;
            p->pending=fence;
            stage="wait";
            rc=jobs_.waitForCompletion(2,&completion,50000);
            waitUs=gpuElapsedUs(jobStart);
            if(!jobs_.outstanding(2))p->pending={};
            if(rc!=kIOReturnSuccess)return rc;
            rc=p->packet.invalidateForCpu(0,Frame::JobBytes);
            uint32_t shaderStatus=0;
            if(rc==kIOReturnSuccess)memcpy(&shaderStatus,p->packet.cpu(),sizeof(shaderStatus));
            if(rc!=kIOReturnSuccess || shaderStatus!=1)return kIOReturnIOError;
            __asm__ volatile("dsb sy" ::: "memory");

            if(check) {
                stage="verify";
                const uint64_t verifyStart=mach_absolute_time();
                mismatch=Frame::countMismatches(
                    reinterpret_cast<const uint32_t *>(input->getVirtualAddress()),
                    reinterpret_cast<const volatile uint32_t *>(scanout->getVirtualAddress()));
                verifyUs=gpuElapsedUs(verifyStart);
                if(mismatch)return kIOReturnIOError;
            }
            stage="unmap";
            rc=p->vm.unmap(Frame::SourceIova,Frame::SourceBytes);
            if(rc==kIOReturnSuccess)p->sourceMapped=false;
            if(rc==kIOReturnSuccess)rc=mmu_.flushRange(p->as,Frame::SourceIova,Frame::SourceBytes);
            if(rc==kIOReturnSuccess)stage="complete";
            return rc;
        }();
        OSSafeReleaseNULL(input);
        if(status==kIOReturnSuccess) {
            status=source->complete(kIODirectionOut);
            if(status==kIOReturnSuccess) {
                p->source->release();
                p->source=nullptr;
                const uint64_t frame=++p->frames;
                if(frame<=12 || frame%120==0)
                    IOLog("M164 mali: display frame=%llu fence=0x%llx spans=%lu setup_us=%llu wait_us=%llu verify_us=%llu total_us=%llu checked=%u mismatches=%u\n",
                        frame,(UINT64_C(2)<<32)|fence.seqno,static_cast<unsigned long>(segments),
                        setupUs,waitUs,verifyUs,gpuElapsedUs(start),check?Frame::Width*Frame::Height:0,mismatch);
                return status;
            }
        }
        // Do not return a failed frame to the producer. Until the AS is
        // disabled, keep both the wired IOSurface and its page tables alive.
        const bool inFlight=p->pending.valid();
        NativeMaliMmuFault fault{};
        if(fence.valid())(void)mmu_.readFault(p->as,&fault);
        const uint64_t failures=++p->frames;
        const IOReturn closed=closePresenter();
        if(failures<=8 || failures%120==0)
            IOLog("M164 mali: display failed n=%llu stage=%s status=0x%x js=0x%x as_fault=0x%x pending=%u cleanup=0x%x mismatches=%u\n",
                failures,stage,status,completion.jsStatus,fault.status,inFlight?1u:0u,closed,mismatch);
        if(closed==kIOReturnSuccess) {
            presenterFailed_=true;
            return kIOReturnUnsupported; // The caller may safely use its existing CPU presenter.
        }
        return closed; // Keep the source pinned if DMA could still be in flight.
    }();
    IOLockUnlock(sessionLock_);
    return ret;
}

extern "C" IOReturn sprd_mali_present(IOMemoryDescriptor *source,IOMemoryMap *scanout,
    uint64_t sourceStride,uint64_t destinationStride,uint32_t width,uint32_t height)
{
    if(!source || !scanout || !scanout->getVirtualAddress() ||
        !SprdMaliPresent::supported(width,height,sourceStride,source->getLength(),
            scanout->getPhysicalAddress(),destinationStride,scanout->getLength()))
        return kIOReturnUnsupported;
    OSDictionary *query=IOService::serviceMatching("NativeSprdMaliService");
    if(!query)return kIOReturnNoMemory;
    IOService *match=IOService::copyMatchingService(query);
    query->release();
    auto *mali=OSDynamicCast(NativeSprdMaliService,match);
    const IOReturn status=!mali?kIOReturnUnsupported:mali->presentFrame(source,scanout);
    OSSafeReleaseNULL(match);
    return status;
}

IOReturn NativeSprdMaliService::exerciseSession()
{
    IOUserClient *client=nullptr,*other=nullptr;
    IOMemoryDescriptor *memory=nullptr;
    IOMemoryMap *mapping=nullptr;
    IOReturn result=newUserClient(kernel_task,nullptr,0,nullptr,&client);
    NativeMaliAbiQuery query{},queryReply{};
    query.abi_version=NativeMaliAbiVersion;
    if(result==kIOReturnSuccess)result=sessionCall(client,NativeMaliMethodQuery,
        &query,sizeof(query),&queryReply,sizeof(queryReply));
    NativeMaliAbiDeviceInfo info{};
    if(result==kIOReturnSuccess)result=sessionCall(client,NativeMaliMethodGetDeviceInfo,
        nullptr,0,&info,sizeof(info));
    if(result==kIOReturnSuccess && (info.gpu_id!=0x74021000 ||
        queryReply.input_bits!=observation_.ias || queryReply.output_bits!=observation_.oas))result=kIOReturnIOError;
    IOLog("M153 mali: client query result=0x%x abi=%u gpu=0x%x bits=%u/%u\n",
        result,queryReply.abi_version,info.gpu_id,queryReply.input_bits,queryReply.output_bits);
    NativeMaliAbiBoAlloc allocation{SprdMaliRender::BufferBytes};
    NativeMaliAbiBoAllocReply allocated{};
    if(result==kIOReturnSuccess)result=sessionCall(client,NativeMaliMethodBoAlloc,
        &allocation,sizeof(allocation),&allocated,sizeof(allocated));
    constexpr uint64_t gpu=0x1000000;
    NativeMaliAbiBoMap gpuMap{allocated.handle,gpu,NativeMaliBoRead|NativeMaliBoWrite,0};
    NativeMaliAbiBoMapReply mapped{};
    if(result==kIOReturnSuccess)result=sessionCall(client,NativeMaliMethodBoMap,
        &gpuMap,sizeof(gpuMap),&mapped,sizeof(mapped));
    NativeMaliAbiBoCpuMap cpuMap{allocated.handle};
    NativeMaliAbiBoCpuMapReply cpuMapped{};
    if(result==kIOReturnSuccess)result=sessionCall(client,NativeMaliMethodBoCpuMap,
        &cpuMap,sizeof(cpuMap),&cpuMapped,sizeof(cpuMapped));
    IOOptionBits options=0;
    if(result==kIOReturnSuccess)result=client->clientMemoryForType(cpuMapped.token,&options,&memory);
    if(result==kIOReturnSuccess) {
        mapping=memory->map(options);
        if(!mapping || !mapping->getVirtualAddress() ||
            mapping->getLength()<SprdMaliRender::BufferBytes)result=kIOReturnNoMemory;
    }
    auto *cpu=mapping?reinterpret_cast<uint8_t *>(mapping->getVirtualAddress()):nullptr;
    if(result==kIOReturnSuccess)result=newUserClient(kernel_task,nullptr,0,nullptr,&other);
    IOReturn isolated=kIOReturnNotReady;
    if(result==kIOReturnSuccess) {
        NativeMaliAbiSubmit invalid{gpu,0,0,0,0};
        NativeMaliAbiSubmitReply ignored{};
        isolated=sessionCall(other,NativeMaliMethodSubmit,
            &invalid,sizeof(invalid),&ignored,sizeof(ignored));
        if(isolated!=kIOReturnBadArgument)result=kIOReturnIOError;
    }
    IOLog("M153 mali: client BO result=0x%x bytes=%llu isolation=0x%x\n",
        result,cpuMapped.bytes,isolated);
    if(cpu)memset(cpu,0xa5,SprdMaliRender::BufferBytes);
    for(unsigned pass=0;pass<4 && result==kIOReturnSuccess;++pass) {
        if(!SprdMaliRender::build(cpu,SprdMaliRender::BufferBytes,gpu,pass))
            {result=kIOReturnBadArgument;break;}
        NativeMaliAbiSubmit submit{gpu,0,0,0,0};
        NativeMaliAbiSubmitReply submitted{};
        result=sessionCall(client,NativeMaliMethodSubmit,&submit,sizeof(submit),
            &submitted,sizeof(submitted));
        NativeMaliAbiWaitFence wait{submitted.fence,100000000ULL};
        if(result==kIOReturnSuccess)result=sessionCall(client,NativeMaliMethodWaitFence,
            &wait,sizeof(wait),nullptr,0);
        IOLog("M153 mali: client fragment pass=%u result=0x%x fence=0x%llx\n",
            pass,result,submitted.fence);
    }
    unsigned mismatches=0,guards=0;
    if(result==kIOReturnSuccess) {
        auto *pixels=reinterpret_cast<volatile uint32_t *>(cpu+SprdMaliRender::ImageOffset);
        for(unsigned y=0;y<SprdMaliRender::Height;++y)
            for(unsigned x=0;x<SprdMaliRender::Width;++x)
                mismatches+=pixels[y*SprdMaliRender::Width+x]!=SprdMaliRender::expected(x,y);
        for(size_t i=0x4000;i<SprdMaliRender::ImageOffset;++i)guards+=cpu[i]!=0xa5;
        for(size_t i=SprdMaliRender::ImageOffset+SprdMaliRender::ImageBytes;
            i<SprdMaliRender::BufferBytes;++i)guards+=cpu[i]!=0xa5;
        if(mismatches || guards)result=kIOReturnIOError;
    }
    NativeMaliAbiStatus status{};
    if(result==kIOReturnSuccess)result=sessionCall(client,NativeMaliMethodGetStatus,
        nullptr,0,&status,sizeof(status));
    if(mapping)mapping->release();
    if(memory)memory->release();
    IOReturn cleanup=kIOReturnSuccess;
    if(client && cpuMapped.token) {
        NativeMaliAbiBoCpuUnmap unmap{allocated.handle,cpuMapped.token,0};
        cleanup=sessionCall(client,NativeMaliMethodBoCpuUnmap,&unmap,sizeof(unmap),nullptr,0);
    }
    if(client && cleanup==kIOReturnSuccess && allocated.handle) {
        NativeMaliAbiBoFree freeBo{allocated.handle};
        cleanup=sessionCall(client,NativeMaliMethodBoFree,&freeBo,sizeof(freeBo),nullptr,0);
    }
    if(other) {
        const IOReturn closed=other->clientClose();
        if(cleanup==kIOReturnSuccess)cleanup=closed;
        other->release();
    }
    if(client) {
        const IOReturn closed=client->clientClose();
        if(cleanup==kIOReturnSuccess)cleanup=closed;
        client->release();
    }
    if(result==kIOReturnSuccess)result=cleanup;
    IOLog("M153 mali: client image result=0x%x pixels=4096 mismatches=%u guards=%u faults=%llu resets=%llu cleanup=0x%x sessions=%u\n",
        result,mismatches,guards,status.faults,status.resets,cleanup,sessions_);
    return result;
}

IOReturn NativeSprdMaliService::exerciseFullFrame()
{
    namespace Frame=SprdMaliFullFrame;
    constexpr uint64_t gpu=0x2000000;
    IOUserClient *client=nullptr;
    IOMemoryDescriptor *memory=nullptr;
    IOMemoryMap *mapping=nullptr;
    const char *stage="open";
    const uint64_t start=mach_absolute_time();
    IOReturn result=newUserClient(kernel_task,nullptr,0,nullptr,&client);
    unsigned verified=0;
    NativeMaliAbiStatus status{};
    if(result==kIOReturnSuccess)result=[&]() -> IOReturn {
        stage="allocate";
        NativeMaliAbiBoAlloc alloc{Frame::BufferBytes};
        NativeMaliAbiBoAllocReply allocated{};
        IOReturn ret=sessionCall(client,NativeMaliMethodBoAlloc,&alloc,sizeof(alloc),
            &allocated,sizeof(allocated));
        if(ret!=kIOReturnSuccess)return ret;

        stage="gpu-map";
        const NativeMaliAbiBoMap map{allocated.handle,gpu,NativeMaliBoRead|NativeMaliBoWrite,0};
        NativeMaliAbiBoMapReply mapped{};
        ret=sessionCall(client,NativeMaliMethodBoMap,&map,sizeof(map),&mapped,sizeof(mapped));
        if(ret!=kIOReturnSuccess)return ret;
        if(mapped.iova!=gpu)return kIOReturnIOError;

        stage="cpu-map";
        const NativeMaliAbiBoCpuMap cpuMap{allocated.handle};
        NativeMaliAbiBoCpuMapReply cpuMapped{};
        ret=sessionCall(client,NativeMaliMethodBoCpuMap,&cpuMap,sizeof(cpuMap),
            &cpuMapped,sizeof(cpuMapped));
        if(ret!=kIOReturnSuccess)return ret;
        if(cpuMapped.bytes!=Frame::BufferBytes)return kIOReturnIOError;
        IOOptionBits options=0;
        ret=client->clientMemoryForType(cpuMapped.token,&options,&memory);
        if(ret!=kIOReturnSuccess)return ret;
        if(!memory)return kIOReturnIOError;
        mapping=memory->map(options);
        if(!mapping || !mapping->getVirtualAddress() ||
            mapping->getLength()<Frame::BufferBytes)return kIOReturnNoMemory;
        auto *cpu=reinterpret_cast<uint8_t *>(mapping->getVirtualAddress());
        memset(cpu,0xa5,Frame::BufferBytes);
        IOLog("M161 mali: frame target=%ux%u stride=%u bytes=%lu setup_us=%llu\n",
            Frame::Width,Frame::Height,Frame::Stride,
            static_cast<unsigned long>(Frame::ImageBytes),gpuElapsedUs(start));

        for(unsigned pass=0;pass<Frame::Passes;++pass) {
            stage="pack";
            if(!Frame::build(cpu,Frame::BufferBytes,gpu,pass))return kIOReturnBadArgument;
            stage="submit";
            const NativeMaliAbiSubmit submit{gpu,0,0,0,0};
            NativeMaliAbiSubmitReply submitted{};
            const uint64_t submitStart=mach_absolute_time();
            ret=sessionCall(client,NativeMaliMethodSubmit,&submit,sizeof(submit),
                &submitted,sizeof(submitted));
            const uint64_t submitUs=gpuElapsedUs(submitStart);
            if(ret!=kIOReturnSuccess)return ret;

            stage="wait";
            const NativeMaliAbiWaitFence wait{submitted.fence,100000000ULL};
            const uint64_t waitStart=mach_absolute_time();
            ret=sessionCall(client,NativeMaliMethodWaitFence,&wait,sizeof(wait),nullptr,0);
            const uint64_t waitUs=gpuElapsedUs(waitStart);
            if(ret!=kIOReturnSuccess)return ret;

            stage="verify";
            Frame::Check check{};
            const uint64_t verifyStart=mach_absolute_time();
            const bool match=Frame::verify(cpu,Frame::BufferBytes,pass,check);
            IOLog("M161 mali: frame pass=%u fence=0x%llx submit_us=%llu wait_sync_us=%llu verify_us=%llu mismatches=%u padding=%u guards=%u\n",
                pass,submitted.fence,submitUs,waitUs,gpuElapsedUs(verifyStart),
                check.pixels,check.padding,check.guards);
            if(!match)return kIOReturnIOError;

            stage="recheck-fence";
            const NativeMaliAbiWaitFence recheck{submitted.fence,0};
            const uint64_t recheckStart=mach_absolute_time();
            ret=sessionCall(client,NativeMaliMethodWaitFence,&recheck,sizeof(recheck),nullptr,0);
            const uint64_t recheckUs=gpuElapsedUs(recheckStart);
            IOLog("M162 mali: retired fence=0x%llx result=0x%x query_us=%llu\n",
                submitted.fence,ret,recheckUs);
            if(ret!=kIOReturnSuccess)return ret;
            ++verified;
        }
        stage="status";
        return sessionCall(client,NativeMaliMethodGetStatus,nullptr,0,&status,sizeof(status));
    }();
    if(client && result!=kIOReturnSuccess)
        (void)sessionCall(client,NativeMaliMethodGetStatus,nullptr,0,&status,sizeof(status));
    if(mapping)mapping->release();
    if(memory)memory->release();
    const IOReturn cleanup=client?client->clientClose():kIOReturnSuccess;
    if(client)client->release();
    if(result==kIOReturnSuccess) {
        stage="cleanup";
        if(cleanup!=kIOReturnSuccess)result=cleanup;
        else if(sessions_ || status.faults || status.resets)result=kIOReturnIOError;
        else stage="complete";
    }
    IOLog("M161 mali: frame result=0x%x stage=%s verified=%u faults=%llu resets=%llu cleanup=0x%x sessions=%u\n",
        result,stage,verified,status.faults,status.resets,cleanup,sessions_);
    return result;
}

extern "C" IOService *sprd_mali_service_create(uint32_t factorySupplyMicrovolts)
{
    static bool classRegistered=false;
    static bool constructorsRun=false;
    if(!classRegistered) {
        // A failed postModLoad must not rerun placement constructors over
        // metaclasses whose expansion data or dictionary entries already exist.
        if(constructorsRun)return nullptr;
        const OSSymbol *baseName=OSSymbol::withCString("IOAcceleratorES");
        const OSMetaClass *baseMeta=baseName?OSMetaClass::getMetaClassWithName(baseName):nullptr;
        if(baseName)baseName->release();
        OSReturn loadStatus=kOSReturnSuccess;
        if(!baseMeta) {
            loadStatus=OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
            baseName=OSSymbol::withCString("IOAcceleratorES");
            baseMeta=baseName?OSMetaClass::getMetaClassWithName(baseName):nullptr;
            if(baseName)baseName->release();
        }
        IOLog("M194 mali: IOGPUFamily load=0x%x IOAcceleratorES=%p\n",
            static_cast<unsigned>(loadStatus),static_cast<const void *>(baseMeta));
        if(!baseMeta)return nullptr;
        // Deferred constructors need the same registration transaction as
        // OSRuntimeInitializeCPP. Loading the base alone does not open one.
        void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if(!handle) {
            IOLog("M195 mali: preModLoad allocation failed\n");
            return nullptr;
        }
        sprd_mali_service_meta_init();
        constructorsRun=true;
        const OSReturn registrationStatus=OSMetaClass::postModLoad(handle);
        IOLog("M195 mali: postModLoad status=0x%x owner=%s\n",
            static_cast<unsigned>(registrationStatus),g_kernel_kmod_info.name);
        if(registrationStatus!=kOSReturnSuccess)return nullptr;
        const OSSymbol *className=OSSymbol::withCString("NativeSprdMaliService");
        const OSMetaClass *serviceMeta=className?OSMetaClass::getMetaClassWithName(className):nullptr;
        if(className)className->release();
        const OSSymbol *clientName=OSSymbol::withCString("NativeSprdMaliUserClient");
        const OSMetaClass *clientMeta=clientName?OSMetaClass::getMetaClassWithName(clientName):nullptr;
        if(clientName)clientName->release();
        if(!serviceMeta || serviceMeta->getClassSize()!=sizeof(NativeSprdMaliService) ||
            !clientMeta || clientMeta->getClassSize()!=sizeof(NativeSprdMaliUserClient)) {
            IOLog("M195 mali: accelerator/client metaclass validation failed service=%p client=%p\n",
                static_cast<const void *>(serviceMeta),static_cast<const void *>(clientMeta));
            return nullptr;
        }
        classRegistered=true;
        IOLog("M195 mali: IOAcceleratorES subclass registered classSize=%u userClientSize=%u\n",
            serviceMeta->getClassSize(),clientMeta->getClassSize());
    }
    auto *service = new NativeSprdMaliService;
    if(!service)return nullptr;
    if(!service->init()){service->release();return nullptr;}
    service->setFactorySupplyMicrovolts(factorySupplyMicrovolts);
    return service;
}

IOReturn NativeSprdMaliService::exerciseCompute()
{
    namespace Compute=SprdMaliCompute;
    IOUserClient *client=nullptr;
    IOMemoryDescriptor *memory=nullptr;
    IOMemoryMap *mapping=nullptr;
    const char *stage="open";
    unsigned verified=0;
    NativeMaliAbiStatus status{};
    IOReturn result=newUserClient(kernel_task,nullptr,0,nullptr,&client);
    if(result==kIOReturnSuccess)result=[&]() -> IOReturn {
        stage="allocate";
        const NativeMaliAbiBoAlloc alloc{Compute::BufferBytes};
        NativeMaliAbiBoAllocReply allocated{};
        IOReturn ret=sessionCall(client,NativeMaliMethodBoAlloc,&alloc,sizeof(alloc),
            &allocated,sizeof(allocated));
        if(ret!=kIOReturnSuccess)return ret;

        stage="gpu-map";
        const NativeMaliAbiBoMap map{allocated.handle,Compute::Gpu,
            NativeMaliBoRead|NativeMaliBoWrite,0};
        NativeMaliAbiBoMapReply mapped{};
        ret=sessionCall(client,NativeMaliMethodBoMap,&map,sizeof(map),&mapped,sizeof(mapped));
        if(ret!=kIOReturnSuccess)return ret;
        if(mapped.iova!=Compute::Gpu)return kIOReturnIOError;

        stage="cpu-map";
        const NativeMaliAbiBoCpuMap cpuMap{allocated.handle};
        NativeMaliAbiBoCpuMapReply cpuMapped{};
        ret=sessionCall(client,NativeMaliMethodBoCpuMap,&cpuMap,sizeof(cpuMap),
            &cpuMapped,sizeof(cpuMapped));
        if(ret!=kIOReturnSuccess)return ret;
        if(cpuMapped.bytes!=Compute::BufferBytes)return kIOReturnIOError;
        IOOptionBits options=0;
        ret=client->clientMemoryForType(cpuMapped.token,&options,&memory);
        if(ret!=kIOReturnSuccess)return ret;
        if(!memory)return kIOReturnIOError;
        mapping=memory->map(options);
        if(!mapping || !mapping->getVirtualAddress() ||
            mapping->getLength()<Compute::BufferBytes)return kIOReturnNoMemory;
        auto *cpu=reinterpret_cast<uint8_t *>(mapping->getVirtualAddress());

        for(unsigned pass=0;pass<Compute::Passes;++pass) {
            stage="prepare";
            if(!Compute::prepare(cpu,Compute::BufferBytes,pass))return kIOReturnBadArgument;
            stage="submit";
            const NativeMaliAbiSubmit submit{Compute::Gpu,0,0,0,0};
            NativeMaliAbiSubmitReply submitted{};
            const uint64_t start=mach_absolute_time();
            ret=sessionCall(client,NativeMaliMethodSubmit,&submit,sizeof(submit),
                &submitted,sizeof(submitted));
            if(ret!=kIOReturnSuccess)return ret;
            const unsigned slot=static_cast<unsigned>(submitted.fence>>32);
            IOLog("M163 mali: compute issued pass=%u slot=%u fence=0x%llx\n",
                pass,slot,submitted.fence);
            stage="wait";
            const NativeMaliAbiWaitFence wait{submitted.fence,100000000ULL};
            ret=sessionCall(client,NativeMaliMethodWaitFence,&wait,sizeof(wait),nullptr,0);
            const uint64_t elapsedUs=gpuElapsedUs(start);
            if(ret!=kIOReturnSuccess)return ret;
            if(slot!=2) {
                stage="slot";
                return kIOReturnIOError;
            }
            stage="verify";
            Compute::Check check{};
            const bool match=Compute::verify(cpu,Compute::BufferBytes,pass,check);
            uint32_t jobStatus=0;
            memcpy(&jobStatus,cpu,sizeof(jobStatus));
            IOLog("M163 mali: compute pass=%u checked=%u submit_wait_us=%llu status=0x%x mismatches=%u input_changed=%u guards=%u first_bad=%u\n",
                pass,static_cast<unsigned>(Compute::Words),elapsedUs,jobStatus,
                check.mismatches,check.inputChanged,check.guards,check.firstBad);
            if(!match)return kIOReturnIOError;
            ++verified;
        }
        stage="status";
        return sessionCall(client,NativeMaliMethodGetStatus,nullptr,0,&status,sizeof(status));
    }();
    if(client && result!=kIOReturnSuccess)
        (void)sessionCall(client,NativeMaliMethodGetStatus,nullptr,0,&status,sizeof(status));
    if(mapping)mapping->release();
    if(memory)memory->release();
    const IOReturn cleanup=client?client->clientClose():kIOReturnSuccess;
    if(client)client->release();
    if(result==kIOReturnSuccess) {
        stage="cleanup";
        if(cleanup!=kIOReturnSuccess)result=cleanup;
        else if(sessions_ || status.faults || status.resets)result=kIOReturnIOError;
        else stage="complete";
    }
    IOLog("M163 mali: compute result=0x%x stage=%s verified=%u faults=%llu resets=%llu cleanup=0x%x sessions=%u\n",
        result,stage,verified,status.faults,status.resets,cleanup,sessions_);
    return result;
}
