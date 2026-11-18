// SPDX-License-Identifier: GPL-2.0-only
// SC2730/UMS512 board backend. The CoreBrightness property/fade interface is
// adapted from UniXNU-sheng/package/AppleARMBacklight.cpp (BSD-2-Clause-Patent).
// No Qualcomm clocks, I2C engines or backlight register values are reused.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include "SprdPwm.h"
#include "FactoryBacklight.h"

namespace {
struct BacklightRegisters {
    volatile uint8_t *base=nullptr;
    uint32_t read32(unsigned offset) {uint32_t v=*(volatile uint32_t *)(base+offset);__asm__ volatile("dsb ld":::"memory");return v;}
    void write32(unsigned offset,uint32_t v) {__asm__ volatile("dsb st":::"memory");*(volatile uint32_t *)(base+offset)=v;__asm__ volatile("dsb sy":::"memory");}
};
}

class NativeSprdBacklight : public IOService {
    OSDeclareDefaultStructors(NativeSprdBacklight)
    IOMemoryMap *maps[3]={};BacklightRegisters regs[3];
    SprdBus::Pwm<BacklightRegisters> pwm{regs[0],2};SprdBus::PwmState saved;
    IOWorkLoop *loop=nullptr;IOTimerEventSource *request=nullptr,*fade=nullptr;
    uint32_t addedGate=0,level=0,from=0,goal=0;
    uint64_t fadeStart=0,fadeDuration=0,pending=0;
    unsigned sets=0,errors=0;bool power=true,prepared=false,pm=false,started=false;
    // IOService has exactly 168 slots in the checked 21H461/source ABI.
    // Reuse the upstream CoreBrightness conformance bridge, but copy only
    // the qualified extent, rather than its unbounded 320-slot read.
    uintptr_t bridge[168]={};uintptr_t *ownVtable=nullptr;
    static inline const OSMetaClass *appleClass=nullptr;
    static const OSMetaClass *coreBrightnessClass(const OSMetaClassBase *) {return appleClass;}
    bool conformance() {
        const OSSymbol *name=OSSymbol::withCString("AppleARMBacklight");
        appleClass=name?OSMetaClass::getMetaClassWithName(name):nullptr;
        OSSafeReleaseNULL(name);
        // This extent is observed in the shipped AppleARMPlatform metaclass.
        if(!appleClass || appleClass->getClassSize()!=0x370)return false;
        ownVtable=*reinterpret_cast<uintptr_t **>(this);
        memcpy(bridge,ownVtable,sizeof bridge);
        bridge[7]=reinterpret_cast<uintptr_t>(&coreBrightnessClass);
        __asm__ volatile("dmb ish":::"memory");
        *reinterpret_cast<uintptr_t **>(this)=bridge;
        return true;
    }
    bool map(unsigned index,uint64_t base,uint64_t length) {
        IODeviceMemory *memory=IODeviceMemory::withRange(base,length);
        maps[index]=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        OSSafeReleaseNULL(memory);
        regs[index].base=maps[index]?(volatile uint8_t *)maps[index]->getVirtualAddress():nullptr;
        return regs[index].base!=nullptr;
    }
    void publishLevel() {
        OSDictionary *brightness=OSDictionary::withCapacity(3),*parameters=OSDictionary::withCapacity(1);
        OSNumber *min=OSNumber::withNumber(0ULL,32),*max=OSNumber::withNumber(255ULL,32),*value=OSNumber::withNumber(level,32);
        if(brightness && parameters && min && max && value) {
            brightness->setObject("min",min);brightness->setObject("max",max);brightness->setObject("value",value);
            parameters->setObject("brightness",brightness);setProperty("IODisplayParameters",parameters);
        }
        OSSafeReleaseNULL(min);OSSafeReleaseNULL(max);OSSafeReleaseNULL(value);
        OSSafeReleaseNULL(brightness);OSSafeReleaseNULL(parameters);
    }
    bool apply(unsigned desired) {
        unsigned raw=power?SprdBus::backlightDuty(desired):0;
        SprdBus::Result r=pwm.duty(raw,power && raw!=0);
        if(r!=SprdBus::Ok) {
            if(errors++<8) {
                IOLog("M104 backlight: PWM2 readback failed result=%d level=%u\n",r,desired);
                const auto &t=pwm.diagnostic();
                IOLog("M110 backlight: phase=%u requested=%u raw=%u enabled=%u before=%x/%x/%x/%x after=%x/%x/%x/%x gate=%x mux=%x\n",
                    t.phase,desired,raw,power,t.before.prescale,t.before.mod,t.before.duty,t.before.enable,
                    t.after.prescale,t.after.mod,t.after.duty,t.after.enable,regs[1].read32(8),regs[2].read32(0x240));
            }
            setProperty("HardwareWriteError",(unsigned)r,32);return false;
        }
        level=desired;publishLevel();
        if(sets<8 || !(sets%64))IOLog("M104 backlight: set=%u logical=%u duty=%u power=%u readback=ok\n",sets,desired,raw,power);
        return true;
    }
    static void fadeTick(OSObject *o,IOTimerEventSource *sender) {
        auto *self=static_cast<NativeSprdBacklight *>(o);
        uint64_t elapsed=mach_absolute_time()-self->fadeStart;unsigned next=self->goal;
        if(elapsed<self->fadeDuration)next=(unsigned)((int64_t)self->from+
            ((int64_t)self->goal-self->from)*(int64_t)elapsed/(int64_t)self->fadeDuration);
        if(next!=self->level && !self->apply(next))return;
        if(next!=self->goal)sender->setTimeoutMS(16);
    }
    static void requestTick(OSObject *o,IOTimerEventSource *) {
        auto *self=static_cast<NativeSprdBacklight *>(o);
        uint64_t v=__atomic_exchange_n(&self->pending,0,__ATOMIC_ACQ_REL);
        if(!(v>>63))return;
        self->fade->cancelTimeout();self->sets++;self->from=self->level;self->goal=(unsigned)((v>>32)&255);
        self->fadeStart=mach_absolute_time();nanoseconds_to_absolutetime((uint64_t)(uint32_t)v*NSEC_PER_MSEC,&self->fadeDuration);
        fadeTick(self,self->fade);
    }
    static IOReturn powerChange(OSObject *o,void *v,void *,void *,void *) {
        auto *self=static_cast<NativeSprdBacklight *>(o);self->power=(uintptr_t)v!=0;
        return self->apply(self->level)?kIOReturnSuccess:kIOReturnIOError;
    }
public:
    IOWorkLoop *getWorkLoop() const override {return loop;}
    bool start(IOService *provider) override {
        if(!IOService::start(provider))return false;started=true;
        if(!map(0,0x32260000,0x1000) || !map(1,0x327d0000,0x3000) || !map(2,0x32080000,0x1000))return false;
        addedGate=(~regs[1].read32(8))&(1u<<27);
        if(addedGate)regs[1].write32(0x1008,addedGate);
        if(!(regs[1].read32(8)&(1u<<27)))return false;
        SprdBus::Result r=pwm.read(saved);
        IOLog("M104 backlight: takeover result=%d mux=0x%x prescale=%u mod=%u duty=%u enable=%u\n",
            r,regs[2].read32(0x240),saved.prescale,saved.mod,saved.duty,saved.enable);
        if(r!=SprdBus::Ok)return false;
        prepared=true;
        // The original PWM probe selects clk_parent=ext26m for every channel.
        // Qualify the inherited mux before applying this board's75us recipe.
        const uint32_t mux=regs[2].read32(0x240);
        if((mux&7)!=1){IOLog("M115 backlight: unqualified PWM2 parent mux=0x%x\n",mux);return false;}
        r=pwm.configureFactory(26000000,75000);
        const auto &wave=pwm.diagnostic();
        IOLog("M115 backlight: factory75us result=%d before=%x/%x/%x/%x div=%x pattern=%x/%x after=%x/%x/%x/%x div=%x pattern=%x/%x mux=%x\n",
            r,wave.before.prescale,wave.before.mod,wave.before.duty,wave.before.enable,wave.before.division,wave.before.patternLow,wave.before.patternHigh,
            wave.after.prescale,wave.after.mod,wave.after.duty,wave.after.enable,wave.after.division,wave.after.patternLow,wave.after.patternHigh,mux);
        if(r!=SprdBus::Ok)return false;
        const unsigned inherited=SprdBus::backlightLevel(saved.enable?saved.duty:0);
        /* cboot leaves PWM2 at raw duty 25 (about 10% of the 8-bit range),
         * which makes the first panel frame effectively unreadable. Keep an
         * explicitly-off waveform off, but start an enabled panel at a
         * calibrated 50% logical floor; later CoreBrightness requests still
         * set the exact requested level. */
        constexpr unsigned kStartupBrightnessFloor=128;
        level=from=goal=saved.enable && inherited<kStartupBrightnessFloor
            ? kStartupBrightnessFloor : inherited;
        if(level!=inherited) {
            if(!apply(level))return false;
            IOLog("M150 backlight: startup floor logical=%u inherited=%u raw=%u\n",
                level,inherited,SprdBus::backlightDuty(level));
        }
        loop=IOWorkLoop::workLoop();request=IOTimerEventSource::timerEventSource(this,requestTick);fade=IOTimerEventSource::timerEventSource(this,fadeTick);
        if(!loop || !request || !fade || loop->addEventSource(request) || loop->addEventSource(fade))return false;
        if(!conformance()) {IOLog("M104 backlight: original AppleARMBacklight extent unavailable\n");return false;}
        setName("backlight");setProperty("Built-In",true);setProperty("NativeSandboxGrantProcess","backboardd");
        setProperty("backlight-control",true);setProperty("HardwareBackend","UMS512 PWM2, factory 1h190 curve");publishLevel();
        PMinit();pm=true;provider->joinPMtree(this);
        static IOPMPowerState states[2]={};states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable|kIOPMPowerOn;
        if(registerPowerDriver(this,states,2)!=kIOReturnSuccess)return false;
        changePowerStateToPriv(1);registerService();
        IOLog("M104 backlight: published PWM2 initial=%u raw=%u original_conformance=1\n",level,saved.duty);
        return true;
    }
    IOReturn setProperties(OSObject *object) override {
        auto *dict=OSDynamicCast(OSDictionary,object);if(!dict)return kIOReturnBadArgument;
        auto *value=OSDynamicCast(OSNumber,dict->getObject("brightness"));if(!value)return kIOReturnBadArgument;
        uint64_t requested=value->unsigned64BitValue();if(requested>255)return kIOReturnBadArgument;
        auto *period=OSDynamicCast(OSNumber,dict->getObject("FadePeriod"));
        uint64_t ms=period?(period->unsigned64BitValue()*1000)>>16:0;if(ms>5000)ms=5000;
        __atomic_store_n(&pending,(1ULL<<63)|(requested<<32)|ms,__ATOMIC_RELEASE);
        request->setTimeoutUS(1);return kIOReturnSuccess;
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *) override {
        if(ordinal>1 || !loop || !prepared)return kIOReturnUnsupported;
        IOReturn r=loop->runAction(powerChange,this,(void *)(uintptr_t)ordinal);
        return r==kIOReturnSuccess?kIOPMAckImplied:r;
    }
    void stop(IOService *provider) override {
        if(request)request->cancelTimeout();if(fade)fade->cancelTimeout();
        if(pm){PMstop();pm=false;}
        if(loop){if(request)loop->removeEventSource(request);if(fade)loop->removeEventSource(fade);}
        OSSafeReleaseNULL(request);OSSafeReleaseNULL(fade);OSSafeReleaseNULL(loop);
        if(prepared){IOLog("M104 backlight: restore result=%d\n",pwm.restore(saved));prepared=false;}
        if(addedGate && regs[1].base)regs[1].write32(0x2008,addedGate);addedGate=0;
        for(auto &m:maps)OSSafeReleaseNULL(m);
        if(ownVtable){*reinterpret_cast<uintptr_t **>(this)=ownVtable;ownVtable=nullptr;}
        if(started){IOService::stop(provider);started=false;}
    }
};
OSDefineMetaClassAndStructors(NativeSprdBacklight,IOService)

static IOService *sprdBacklightCreate(IOService *provider) {
    auto *device=new NativeSprdBacklight;
    if(!device)return nullptr;
    if(!device->init()){device->release();return nullptr;}
    if(!device->attach(provider)){device->release();return nullptr;}
    if(!device->start(provider)){device->stop(provider);device->detach(provider);device->release();return nullptr;}
    return device;
}
