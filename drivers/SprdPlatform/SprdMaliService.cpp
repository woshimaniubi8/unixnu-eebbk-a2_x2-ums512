// SPDX-License-Identifier: GPL-2.0-only
// Persistent board-owned Mali service. M149 deliberately stops at a real
// IOService and hardware lifetime; user-client/Metal publication is M150.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include "mali/NativeMaliGpuCore.h"
#include "mali/NativeMaliJobSlot.h"
#include "mali/NativeMaliMmu.h"

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

} // namespace

class NativeSprdMaliService final : public IOService {
    OSDeclareDefaultStructors(NativeSprdMaliService)
    Window windows_[4] = {
        {0x60000000, 0x100000}, /* Mali core/JM/MMU */
        {0x327e0000, 0x1000},   /* GPU PMU gate */
        {0x327d0000, 0x3000},   /* AON APB gate */
        {0x60100000, 0x3000}    /* GPU clock */
    };
    NativeMaliPower *power_ = nullptr;
    NativeMaliGpuCore core_;
    NativeMaliMmu mmu_;
    NativeMaliJobSlot jobs_;
    NativeMaliGpuObservation observation_{};
    bool corePrepared_ = false;
    bool resetDone_ = false;
    bool powered_ = false;
    bool mmuPrepared_ = false;
    bool jobsPrepared_ = false;
    bool ready_ = false;

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
        if(power_) {
            delete power_;
            power_ = nullptr;
        }
        ready_ = false;
        corePrepared_ = false;
    }

public:
    bool start(IOService *provider) override
    {
        if(!IOService::start(provider))return false;
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
            IOLog("M149 mali: service map failed=0x%x\n", mapping);
            stop(provider);
            return false;
        }

        power_ = new NativeMaliPower(
            reinterpret_cast<volatile uint8_t *>(windows_[1].map->getVirtualAddress()),
            reinterpret_cast<volatile uint8_t *>(windows_[2].map->getVirtualAddress()),
            reinterpret_cast<volatile uint8_t *>(windows_[3].map->getVirtualAddress()));
        if(!power_) {
            IOLog("M149 mali: service power allocation failed\n");
            stop(provider);
            return false;
        }
        IOReturn result = core_.prepare(&windows_[0].regs, power_);
        corePrepared_ = result == kIOReturnSuccess;
        if(result == kIOReturnSuccess)result = core_.probe(&observation_);
        IOLog("M149 mali: service probe result=0x%x id=0x%x js=0x%x as=0x%x shader=0x%llx\n",
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
            IOLog("M149 mali: service start failed=0x%x\n", result);
            stop(provider);
            return false;
        }
        setProperty("gpu-id", observation_.gpuId, 32);
        setProperty("job-slots", observation_.jsPresent, 32);
        setProperty("service-ready", 1, 32);
        setName("Mali-G52");
        registerService();
        ready_ = true;
        IOLog("M149 mali: service published ready=1 id=0x%x js=0x%x\n",
            observation_.gpuId, observation_.jsPresent);
        return true;
    }

    void stop(IOService *provider) override
    {
        IOLog("M149 mali: service stop ready=%u\n", ready_ ? 1u : 0u);
        stopHardware();
        releaseWindows();
        IOService::stop(provider);
    }
};

OSDefineMetaClassAndStructors(NativeSprdMaliService, IOService)

extern "C" IOService *sprd_mali_service_create()
{
    auto *service = new NativeSprdMaliService;
    return service && service->init() ? service : nullptr;
}
