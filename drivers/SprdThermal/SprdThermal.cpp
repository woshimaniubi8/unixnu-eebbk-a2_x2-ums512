// SPDX-License-Identifier: GPL-2.0-only
// UMS512 thermal acquisition, with original IOHIDEventService event delivery.
// Register protocol/calibration: Spreadtrum sprd_thermal.c and sprd_cache_efuse.c.
// IOHID bridge is qualified for IOHIDFamily-2031.120.4 / ordinary arm64 21H461.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/OSKextLib.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include "ThermalMath.h"

class NativeSprdThermal;
namespace {
constexpr unsigned kPhysical = 12, kSensors = 15, kHidSlots = 219;
constexpr uintptr_t kHidVtable = 0xfffffff005d05e10ULL;
constexpr uintptr_t kHidInit = 0xfffffff004aeb0d8ULL;
constexpr uintptr_t kTemperatureEvent = 0xfffffff004afbdc8ULL;
constexpr unsigned kCopyEventSlot = 201, kDispatchEventSlot = 198;
const uint8_t controller[kPhysical] = {0,0,0,0,0,1,1,1,1,1,1,2};
const uint8_t channel[kPhysical] = {0,1,2,3,4,0,1,2,3,4,5,0};
const uint8_t deltaOffset[kPhysical] = {0x39,0x41,0x40,0x43,0x42,0x3d,0x2f,0x2e,0x29,0x28,0x2b,0x3f};
const uint8_t ratioOffset[3] = {0x38,0x3c,0x3e};
const unsigned count[3] = {5,6,1};
const char *locations[kSensors] = {
    "Unisoc prometheus6 sensor0", "Unisoc prometheus6 sensor1", "Unisoc prometheus7",
    "Unisoc ank0", "Unisoc ank1", "Unisoc gpu", "Unisoc ank2", "Unisoc ank3",
    "Unisoc ank4", "Unisoc ank5", "Unisoc cputop", "Unisoc gpuank2",
    "Unisoc A55 max", "Unisoc A75 max", "Unisoc GPU max"
};
const char *keys[kSensors] = {"TN0i","TN1i","TN2i","TN3i","TN4i","TN5i", "TN6i","TN7i","TN8i","TN9i","TNAi","TNBi","TN0x","TN1x","TN2x"};
const unsigned a55[] = {3,4,6,7,8,9}, a75[] = {0,1,2}, gpu[] = {5,11};
struct Sample { int32_t milliC; uint64_t time; bool valid; };
struct Saved { uint32_t ctrl, interrupts, detection, period, mode, threshold[6]; };
NativeSprdThermal *instance;
IOService *services[kSensors];
void *vtables[kSensors][kHidSlots];
Sample samples[kSensors];
IOLock *sampleLock;

OSObject *makeEvent(IOService *self, uint32_t type, OSObject *, uint32_t)
{
    if (type != 15 || !sampleLock) return nullptr;
    unsigned index = kSensors;
    for (unsigned i = 0; i < kSensors; i++) if (services[i] == self) { index = i; break; }
    if (index == kSensors) return nullptr;
    IOLockLock(sampleLock);
    Sample s = samples[index];
    IOLockUnlock(sampleLock);
    uint64_t age = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - s.time, &age);
    if (!s.valid || age > 3000000000ULL) return nullptr;
    using Factory = OSObject *(*)(uint64_t, int32_t, uint32_t);
    return reinterpret_cast<Factory>(kTemperatureEvent)(s.time, SprdThermalMath::fixedC(s.milliC), 0);
}
uint32_t usagePage(IOService *) { return 0x20; }
uint32_t usage(IOService *) { return 0x31; }
OSObject *product(IOService *self) { return self->getProperty("Product"); }
bool compatible(IOService *p)
{
    auto *d = p ? OSDynamicCast(OSData, p->getProperty("compatible")) : nullptr;
    if (!d) return false;
    const char *b = (const char *)d->getBytesNoCopy();
    unsigned n = d->getLength(), o = 0;
    while (o < n) {
        size_t z = strnlen(b + o, n - o);
        if (z == n - o) break;
        if (!strcmp(b + o, "eebbk,ums512-thermal")) return true;
        o += (unsigned)z + 1;
    }
    return false;
}
}

class NativeSprdThermal : public IOService {
    OSDeclareDefaultStructors(NativeSprdThermal)
    IOMemoryMap *maps[3] = {};
    volatile uint32_t *registers[3] = {}, *aon = nullptr;
    volatile uint8_t *efuse = nullptr;
    SprdThermalMath::Calibration calibration[kPhysical] = {};
    Saved saved[3] = {};
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    uint32_t gatesAdded = 0;
    unsigned configured = 0, ticks = 0, hotCount = 0;
    bool hidStarted[kSensors] = {};

    bool prepareHardware();
    bool publishSensors();
    void restoreHardware();
    static void timerAction(OSObject *owner, IOTimerEventSource *)
    { static_cast<NativeSprdThermal *>(owner)->poll(); }
public:
    IOService *probe(IOService *p, SInt32 *score) override
    { if (!compatible(p)) return nullptr; if (score) *score = 2000; return this; }
    bool start(IOService *p) override;
    void stop(IOService *p) override;
    IOWorkLoop *getWorkLoop() const override { return loop; }
    void poll();
};
OSDefineMetaClassAndStructors(NativeSprdThermal, IOService)

bool NativeSprdThermal::prepareHardware()
{
    // The cache has a four-byte header; nvmem offset N reads cache[4+N].
    // The offset is a byte offset, not an efuse block number (vendor provider).
    unsigned nonzero = 0, nonones = 0;
    for (unsigned i = 0; i < kPhysical; i++) {
        uint8_t delta = efuse[4 + deltaOffset[i]];
        nonzero += delta != 0; nonones += delta != 255;
        uint8_t ratio = efuse[4 + ratioOffset[controller[i]]];
        if (!SprdThermalMath::calibrate(ratio & 1, (ratio >> 1) & 127, delta, calibration[i])) return false;
        IOLog("bbk thm: cal id=%u group=%u channel=%u delta=%u slope=%d offset=%d\n",
              i, controller[i], channel[i], delta, calibration[i].slope, calibration[i].offset);
    }
    if (!nonzero || !nonones) {
        IOLog("bbk thm: invalid/unpopulated efuse cache; no temperature is published\n");
        return false;
    }
    uint32_t before = aon[0x8/4];
    gatesAdded = (~before) & 7;
    if (gatesAdded) aon[0x1008/4] = gatesAdded;
    __asm__ volatile("dsb sy" ::: "memory");
    if ((aon[0x8/4] & 7) != 7) return false;
    for (unsigned g = 0; g < 3; g++) {
        auto *r = registers[g]; auto &s = saved[g];
        s.ctrl=r[0]; s.interrupts=r[1]; s.detection=r[4]; s.period=r[0x4c/4]; s.mode=r[0x50/4];
        for (unsigned c = 0; c < count[g]; c++) s.threshold[c]=r[0x2c/4+c];
        configured = g + 1;
        r[4]=(r[4]&~0xfffffu)|0x800;
        r[0x4c/4]=(r[0x4c/4]&~0xffffu)|0x10;
        r[0x50/4]=(r[0x50/4]&~0xfu)|7;
        uint32_t sensors=0;
        for (unsigned i=0;i<kPhysical;i++) if (controller[i]==g) {
            unsigned c=channel[i]; sensors|=1u<<(c+2);
            r[0x2c/4+c]=(r[0x2c/4+c]&~0xfffffu) |
                (SprdThermalMath::threshold(calibration[i],120000)<<10) |
                 SprdThermalMath::threshold(calibration[i],75000);
        }
        r[0] |= sensors;
        unsigned spin=0;
        while ((r[0]&(1u<<13)) && spin++<70) IODelay(10);
        if (r[0]&(1u<<13)) { IOLog("bbk thm: SET_RDY timeout group=%u\n",g); return false; }
        r[0] |= (1u<<1)|(1u<<12);
        r[1] |= (1u<<26);
        r[0x14/4] = 0x1ffffff; // Vendor SET_RDY: acknowledge latched status before THM_EN.
        r[0] |= 1;
        spin=0;
        while ((r[0x54/4]&1) && spin++<60) IOSleep(10);
        if (r[0x54/4]&1) { IOLog("bbk thm: TEMP_RDY timeout group=%u\n",g); return false; }
        IOLog("bbk thm: group=%u ctrl=0x%08x status=0x%08x channels=%u\n",g,r[0],r[0x54/4],count[g]);
    }
    return true;
}

bool NativeSprdThermal::publishSensors()
{
    if (OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver") != kOSReturnSuccess) {
        IOLog("bbk thm: IOHIDEventDriver load failed\n"); return false;
    }
    // Guard exact ordinary-arm64 entry points before using the target ABI.
    const uint32_t signature[] = {0xa9bc5ff8,0xa90157f6,0xa9024ff4,0xa9037bfd};
    const volatile uint32_t *factoryWords = reinterpret_cast<const volatile uint32_t *>(kTemperatureEvent);
    for (unsigned i=0;i<sizeof(signature)/sizeof(signature[0]);i++)
        if (factoryWords[i]!=signature[i]) {
            IOLog("bbk thm: original temperature factory ABI mismatch\n"); return false;
        }
    const OSSymbol *name=OSSymbol::withCString("IOHIDEventService");
    const OSMetaClass *meta=name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if(name) name->release();
    if(!meta || meta->getClassSize()!=0xc8) { IOLog("bbk thm: IOHIDEventService class extent mismatch\n"); return false; }
    OSArray *pairs=OSArray::withCapacity(1);
    OSDictionary *pair=OSDictionary::withCapacity(2);
    OSNumber *page=OSNumber::withNumber(0x20,32), *use=OSNumber::withNumber(0x31,32);
    if(!pairs || !pair || !page || !use) {
        OSSafeReleaseNULL(pairs); OSSafeReleaseNULL(pair); OSSafeReleaseNULL(page); OSSafeReleaseNULL(use); return false;
    }
    pair->setObject("DeviceUsagePage",page); pair->setObject("DeviceUsage",use);
    pairs->setObject(pair); setProperty("DeviceUsagePairs",pairs); setProperty("Transport","Internal");
    page->release(); use->release(); pair->release(); pairs->release();
    for(unsigned i=0;i<kSensors;i++) {
        // Allocate a real concrete HID class only after its kext has registered.
        // Use the base event-service lifecycle, with only our temperature hooks.
        auto *s=static_cast<IOService *>(OSMetaClass::allocClassWithName("IOHIDEventDriver"));
        if(!s) { IOLog("bbk thm: HID allocation failed id=%u\n",i); return false; }
        void **original=*reinterpret_cast<void ***>(s);
        const volatile uintptr_t *baseSlots = reinterpret_cast<const volatile uintptr_t *>(kHidVtable);
        for(unsigned slot=0;slot<kHidSlots;slot++) vtables[i][slot]=reinterpret_cast<void *>(baseSlots[slot]);
        // Preserve allocation class identity and its sized delete routines.
        vtables[i][0]=original[0]; vtables[i][1]=original[1]; vtables[i][7]=original[7];
        vtables[i][199]=(void *)&usagePage; vtables[i][200]=(void *)&usage;
        vtables[i][178]=(void *)&product;
        vtables[i][kCopyEventSlot]=(void *)&makeEvent;
        *reinterpret_cast<void ***>(s)=vtables[i]; services[i]=s;
        using Init=bool (*)(IOService *,OSDictionary *);
        if(!reinterpret_cast<Init>(kHidInit)(s,nullptr)) return false;
        // The target daemon matches Product strings (0x10000fcf0), not Location.
        s->setName("UnisocThermalSensor"); s->setProperty("Product",locations[i]);
        s->setProperty("SensorName",keys[i]); s->setProperty("Built-In",true);
        s->setProperty("ReportInterval",1000000u,32);
        if(!s->attach(this)) return false;
        if(!s->start(this)) return false;
        hidStarted[i]=true;
        IOLog("bbk thm: HID published id=%u key=%s location=%s\n",i,keys[i],locations[i]);
    }
    return true;
}

bool NativeSprdThermal::start(IOService *p)
{
    if(!compatible(p) || instance || !IOService::start(p)) return false;
    instance=this;
    sampleLock=IOLockAlloc(); loop=IOWorkLoop::workLoop();
    if(!sampleLock || !loop) { stop(p); return false; }
    for(unsigned i=0;i<3;i++) { maps[i]=p->mapDeviceMemoryWithIndex(i); if(!maps[i]) { stop(p); return false; } }
    auto base=maps[0]->getVirtualAddress();
    for(unsigned g=0;g<3;g++) registers[g]=(volatile uint32_t *)(base+g*0x10000);
    aon=(volatile uint32_t *)maps[1]->getVirtualAddress();
    efuse=(volatile uint8_t *)(maps[2]->getVirtualAddress()+0xc00);
    if(!prepareHardware()) { IOLog("bbk thm: hardware preparation failed\n"); stop(p); return false; }
    poll();
    if(!publishSensors()) { IOLog("bbk thm: HID publication failed\n"); stop(p); return false; }
    timer=IOTimerEventSource::timerEventSource(this,timerAction);
    if(!timer || loop->addEventSource(timer)!=kIOReturnSuccess) { stop(p); return false; }
    timer->setTimeoutMS(1000);
    setProperty("PhysicalSensorCount",kPhysical,32); registerService();
    IOLog("bbk thm: ready physical=12 derived=3 interval=1000ms stale=3000ms\n");
    return true;
}

void NativeSprdThermal::poll()
{
    Sample next[kSensors]={}; uint64_t now=mach_absolute_time(); unsigned valid=0; int hottest=-40000;
    for(unsigned i=0;i<kPhysical;i++) {
        auto *r=registers[controller[i]];
        uint32_t raw=r[0x5c/4+channel[i]]&0x3ff;
        next[i].time=now;
        next[i].valid=!(r[0x54/4]&1) && SprdThermalMath::convert(calibration[i],raw,next[i].milliC);
        if(next[i].valid) { valid++; if(next[i].milliC>hottest) hottest=next[i].milliC; }
        if(ticks<3 || ticks%30==0) IOLog("bbk thm: sample id=%u raw=%u milliC=%d valid=%u\n",i,raw,next[i].milliC,next[i].valid);
    }
    const unsigned *groups[]={a55,a75,gpu}; const unsigned sizes[]={6,3,2};
    for(unsigned g=0;g<3;g++) {
        Sample &s=next[kPhysical+g]; s.valid=true; s.milliC=-40000; s.time=now;
        for(unsigned j=0;j<sizes[g];j++) { Sample &v=next[groups[g][j]]; s.valid &= v.valid; if(v.milliC>s.milliC) s.milliC=v.milliC; }
    }
    IOLockLock(sampleLock); memcpy(samples,next,sizeof(samples)); IOLockUnlock(sampleLock);
    for(unsigned i=0;i<kSensors;i++) if(services[i] && hidStarted[i]) {
        OSObject *event=makeEvent(services[i],15,nullptr,0);
        if(event) {
            using Dispatch=void (*)(IOService *,OSObject *,uint32_t);
            reinterpret_cast<Dispatch>(vtables[i][kDispatchEventSlot])(services[i],event,0);
            event->release();
        }
    }
    hotCount=hottest>=110000 ? hotCount+1 : 0;
    if(hotCount>=3) panic("NativeSprdThermal: sustained SoC temperature %d mC (stock critical 110C)",hottest);
    if(ticks<3 || ticks%30==0) IOLog("bbk thm: poll valid=%u/12 hottest=%d sequence=%u\n",valid,hottest,ticks);
    ticks++;
    if(timer) timer->setTimeoutMS(1000);
}

void NativeSprdThermal::restoreHardware()
{
    for(unsigned g=0;g<configured;g++) {
        auto *r=registers[g]; auto &s=saved[g];
        r[0]=s.ctrl; r[1]=s.interrupts; r[4]=s.detection; r[0x4c/4]=s.period; r[0x50/4]=s.mode;
        for(unsigned c=0;c<count[g];c++) r[0x2c/4+c]=s.threshold[c];
    }
    configured=0;
    if(aon && gatesAdded) aon[0x2008/4]=gatesAdded;
    gatesAdded=0;
}
void NativeSprdThermal::stop(IOService *p)
{
    if(timer) { timer->cancelTimeout(); if(loop) loop->removeEventSource(timer); timer->release(); timer=nullptr; }
    for(unsigned i=0;i<kSensors;i++) if(services[i]) {
        if(hidStarted[i]) services[i]->stop(this);
        if(services[i]->getProvider()==this) services[i]->detach(this);
        services[i]->release(); services[i]=nullptr; hidStarted[i]=false;
    }
    restoreHardware();
    for(auto &m:maps) { if(m) m->release(); m=nullptr; }
    if(loop) { loop->release(); loop=nullptr; }
    if(sampleLock) { IOLockFree(sampleLock); sampleLock=nullptr; }
    if(instance==this) instance=nullptr;
    IOService::stop(p);
}
