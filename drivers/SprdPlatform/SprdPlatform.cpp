// SPDX-License-Identifier: GPL-2.0-only
// EEBBK 1h190 hardware acquisition. No Apple register addresses or emulated
// battery values. Register protocol provenance is recorded in README.md.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include "SprdBus.h"
#include "SprdBacklight.h"
#include "SprdTouchPins.h"
#include "SprdFuelGauge.h"
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void sprd_hid_static_init() __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *sprd_hid_allocate(bool);
extern "C" IOReturn sprd_hid_buttons(IOService *,uint8_t);
extern "C" IOReturn sprd_hid_contacts(IOService *,const SprdBus::Frame *,unsigned,unsigned);
extern "C" void sprd_hid_diagnostics(IOService *);
using namespace SprdBus;
namespace {
struct Registers {
    volatile uint8_t *base=nullptr;
    uint32_t read32(unsigned off) {uint32_t v=*(volatile uint32_t *)(base+off);__asm__ volatile("dsb ld":::"memory");return v;}
    void write32(unsigned off,uint32_t v) {__asm__ volatile("dsb st":::"memory");*(volatile uint32_t *)(base+off)=v;__asm__ volatile("dsb sy":::"memory");}
    uint8_t read8(unsigned off) {uint8_t v=base[off];__asm__ volatile("dsb ld":::"memory");return v;}
    void write8(unsigned off,uint8_t v) {__asm__ volatile("dsb st":::"memory");base[off]=v;}
    void delay(unsigned us) {IODelay(us);}
};
bool compatible(IOService *p) {
    OSData *d=p?OSDynamicCast(OSData,p->getProperty("compatible")):nullptr;
    if(!d)return false;
    const char *s=(const char *)d->getBytesNoCopy();unsigned n=d->getLength(),at=0;
    while(at<n) {size_t z=strnlen(s+at,n-at);if(z==n-at)break;
        if(!strcmp(s+at,"eebbk,1h190-platform"))return true;at+=(unsigned)z+1;}
    return false;
}
bool stringProperty(OSDictionary *d,const char *key,const char *text) {
    OSString *s=OSString::withCString(text);if(!s)return false;
    bool ok=d->setObject(key,s);s->release();return ok;
}
}
class NativeSprdPlatform : public IOService {
    OSDeclareDefaultStructors(NativeSprdPlatform)
    IOMemoryMap *maps[7]={}; Registers mmio[7];
    IOMemoryMap *touchPinMap=nullptr;Registers touchPinRegisters;
    IOMemoryMap *fuelLockMap=nullptr;Registers fuelLockRegisters;
    FuelCalibration fuelCal;Result fuelCalibrationResult=Invalid;
    IOPMrootDomain *sleepRoot=nullptr;IOPMDriverAssertionID pollingAssertion=kIOPMUndefinedDriverAssertionID;
    TouchPins<Registers,Registers> touchPins{touchPinRegisters,mmio[1]};
    IOWorkLoop *loop=nullptr;IOTimerEventSource *timer=nullptr;
    IOService *keys=nullptr,*touch=nullptr,*backlight=nullptr;
    OSDictionary *touchPersonality=nullptr;
    bool keysStarted=false,touchStarted=false;
    uint32_t aonAdded[2]={},apAdded=0,clockSaved[2]={};bool clockChanged=false;
    uint32_t gpioSaved[3]={};bool gpioChanged=false,pmicChanged=false,pmicReady=false;
    uint16_t pmicMask=0;unsigned touchKind=0,touchW=0,touchH=0,ticks=0,touchErrors=0,touchReports=0,keyErrors=0;
    uint16_t chipLo=0,chipHi=0;
    uint8_t goodixId[6]={},ftsId[3]={},chargerId=0;
    int goodixResult=Invalid,goodixConfigResult=Invalid,ftsResult[3]={Invalid,Invalid,Invalid},chargerResult=Invalid;
    uint32_t lastPhysicalMask=0xffffffffu;
    Frame lastFrame={};Debounce debounce;
    Adi<Registers> adi{mmio[0]};I2c<Registers> bus3{mmio[4]},bus4{mmio[5]};
    static void tick(OSObject *o,IOTimerEventSource *) {static_cast<NativeSprdPlatform *>(o)->poll();}
    bool prepare();bool publishInput();void identifyTouch();void recoverTouchReset();void prepareFuelGauge();void requirePollingPower();void poll();void readTouch();void restore();
public:
    IOService *probe(IOService *p,SInt32 *score) override {if(!compatible(p))return nullptr;if(score)*score=2200;return this;}
    IOWorkLoop *getWorkLoop() const override {return loop;}
    bool start(IOService *) override;void stop(IOService *) override;
};
OSDefineMetaClassAndStructors(NativeSprdPlatform,IOService)

bool NativeSprdPlatform::prepare() {
    // GPIO gate AON+4 bit1, ADI gate AON+8 bit9; AP_APB I2C3/4 bits12/13.
    // Set/clear aliases preserve concurrent thermal/watchdog clocks.
    for(unsigned i=0;i<2;i++) {
        unsigned off=i?8:4;uint32_t mask=i?(1u<<9):(1u<<1);
        aonAdded[i]=(~mmio[2].read32(off))&mask;
        if(aonAdded[i])mmio[2].write32(0x1000+off,aonAdded[i]);
        if((mmio[2].read32(off)&mask)!=mask)return false;
    }
    apAdded=(~mmio[3].read32(0))&0x3000;if(apAdded)mmio[3].write32(0x1000,apAdded);
    if((mmio[3].read32(0)&0x3000)!=0x3000)return false;
    // Select the factory ext-26m parent, divide by one, before programming SCL.
    for(unsigned i=0;i<2;i++) {unsigned off=0x40+4*i;clockSaved[i]=mmio[6].read32(off);mmio[6].write32(off,clockSaved[i]&~0x703u);}
    clockChanged=true;bus3.configure26000kHz400kHz();bus4.configure26000kHz400kHz();
    const unsigned bank=7*0x80,mask=1u<<12; // Factory volume-down GPIO124.
    gpioSaved[0]=mmio[1].read32(bank+4);gpioSaved[1]=mmio[1].read32(bank+8);gpioSaved[2]=mmio[1].read32(bank+0x28);
    mmio[1].write32(bank+4,gpioSaved[0]|mask);mmio[1].write32(bank+8,gpioSaved[1]&~mask);mmio[1].write32(bank+0x28,gpioSaved[2]|mask);gpioChanged=true;
    Result r=adi.read(0x284,pmicMask);
    if(r==Ok) {pmicChanged=true;r=adi.update(0x284,0x1a,0x1a);pmicReady=r==Ok;}
    uint16_t level=0;r=pmicReady?adi.read(0x280,level):r;
    IOLog("bbk platform: ADIv3 pmic_eic status=%d mask=0x%x data=0x%x gpio124=%u\n",r,pmicMask,level,(mmio[1].read32(bank)>>12)&1);
    if(!pmicReady)return false;
    Result lo=adi.read(0x1800,chipLo),hi=adi.read(0x1804,chipHi);
    IOLog("bbk platform: PMIC chip lo=0x%x hi=0x%x status=%d/%d\n",chipLo,chipHi,lo,hi);
    chargerResult=bus4.readRegister(0x6b,0x0b,1,&chargerId,1);
    IOLog("bbk platform: bq2560x id register=0x%x status=%d (read only)\n",chargerId,chargerResult);
    return true;
}
bool NativeSprdPlatform::publishInput() {
    kern_return_t r=OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if(r){IOLog("bbk input: IOHIDFamily load failed 0x%x\n",r);return false;}
    const OSSymbol *n=OSSymbol::withCString("IOHIDDevice");
    const OSMetaClass *meta=n?OSMetaClass::getMetaClassWithName(n):nullptr;
    if(n)n->release();
    if(!meta || meta->getClassSize()!=0xd0){IOLog("bbk input: original IOHIDDevice extent mismatch\n");return false;}
    // OSKextLib's convenience function defaults startMatchingOpt to
    // kOSKextExcludeAll. The codeless personality must be submitted after its
    // executable owner has registered IOHIDEventDriver's real metaclass.
    OSReturn match=OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",
        false,false,kOSKextExcludeNone,kOSKextExcludeNone,nullptr);
    IOLog("M102 hid: IOHIDEventDriver matching load=0x%x\n",match);
    if(match!=kOSReturnSuccess)return false;
    if(touchKind) {
        // Use the original in-kernel digitizer parser for this exact physical
        // device. The generic iOS DriverKit personality otherwise matches it
        // and leaves IOHIDInterface busy waiting for a missing user driver.
        // Match scope is Vendor1782/Product1902, not all HID interfaces.
        touchPersonality=OSDictionary::withCapacity(8);OSDictionary *properties=OSDictionary::withCapacity(2);
        OSArray *drivers=OSArray::withCapacity(1);OSNumber *vendor=OSNumber::withNumber(0x1782ULL,32);
        OSNumber *product=OSNumber::withNumber(0x1902ULL,32),*score=OSNumber::withNumber(100000ULL,32);
        bool ok=touchPersonality && properties && drivers && vendor && product && score;
        if(ok)ok=properties->setObject("VendorID",vendor) && properties->setObject("ProductID",product) &&
            stringProperty(touchPersonality,"CFBundleIdentifier","com.apple.iokit.IOHIDFamily") &&
            stringProperty(touchPersonality,"IOClass","IOHIDEventDriver") &&
            stringProperty(touchPersonality,"IOProviderClass","IOHIDInterface") &&
            stringProperty(touchPersonality,"IOMatchCategory","IODefaultMatchCategory") &&
            touchPersonality->setObject("IOPropertyMatch",properties) && touchPersonality->setObject("IOProbeScore",score) &&
            touchPersonality->setObject("NativeSprdPhysicalTouch",kOSBooleanTrue) && drivers->setObject(touchPersonality);
        if(ok)ok=gIOCatalogue->addDrivers(drivers,true);
        OSSafeReleaseNULL(properties);OSSafeReleaseNULL(drivers);OSSafeReleaseNULL(vendor);OSSafeReleaseNULL(product);OSSafeReleaseNULL(score);
        IOLog("M107 touch: original kernel event-driver personality installed=%u vendor=1782 product=1902\n",ok);
        if(!ok)return false;
    }
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!handle)return false;
    sprd_hid_static_init();OSReturn status=OSMetaClass::postModLoad(handle);
    if(status){IOLog("bbk input: own metaclass registration status=0x%x\n",status);return false;}
    keys=sprd_hid_allocate(false);
    if(!keys || !keys->init() || !keys->attach(this))return false;
    keysStarted=keys->start(this);if(!keysStarted)return false;
    IOLog("bbk input: physical keys HID published (power/vol+/vol-)\n");
    if(touchKind) {
        touch=sprd_hid_allocate(true);
        if(!touch || !touch->init() || !touch->attach(this))return false;
        touchStarted=touch->start(this);if(!touchStarted)return false;
        IOLog("bbk input: touch HID published kind=%u dimensions=%ux%u\n",touchKind,touchW,touchH);
    }
    return true;
}
void NativeSprdPlatform::prepareFuelGauge() {
    if(chipHi!=0x2730){IOLog("M106 fuel: PMIC variant not qualified; calibration not read\n");return;}
    IODeviceMemory *memory=IODeviceMemory::withRange(0x327f0000,0x1000);
    fuelLockMap=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    OSSafeReleaseNULL(memory);
    if(!fuelLockMap){IOLog("M106 fuel: hwspinlock map failed\n");return;}
    fuelLockRegisters.base=(volatile uint8_t *)fuelLockMap->getVirtualAddress();
    FuelGaugeCalibration<Adi<Registers>,Registers,Registers> reader(adi,fuelLockRegisters,mmio[2]);
    fuelCalibrationResult=reader.read(fuelCal);
    const FuelReadTrace &trace=reader.diagnostic();
    IOLog("M107 fuel: phase=%u gate=0x%x control=0x%x token12=0x%x status=0x%x module=0x%x mode=0x%x\n",
        trace.phase,trace.gate,trace.control,trace.token,trace.status,trace.module,trace.mode);
    IOLog("M106 fuel: calibration status=%d efuse3=0x%x voltage_adc=%u current_adc=%u (factory 20mOhm/10mOhm)\n",
        fuelCalibrationResult,fuelCal.efuse,fuelCal.voltageAdcPerVolt,fuelCal.currentAdcPerAmp);
    if(fuelCalibrationResult==Ok){setProperty("FGUVoltageAdcPerVolt",fuelCal.voltageAdcPerVolt,32);setProperty("FGUCurrentAdcPerAmp",fuelCal.currentAdcPerAmp,32);}
}
void NativeSprdPlatform::requirePollingPower() {
    if(pollingAssertion || ticks%100)return;
    sleepRoot=getPMRootDomain();
    if(sleepRoot)pollingAssertion=sleepRoot->createPMAssertion(kIOPMDriverAssertionCPUBit,kIOPMDriverAssertionLevelOn,this,
        "org.unixnu.eebbk.1h190.polled-input-requires-running-cpu");
    // The qualified ArmCpu quiesce disables its timer and GIC CPU interface;
    // it has no system-resume entry. Do not advertise a wake-capable device
    // merely because a polled key works while the CPU is running. The normal
    // PM assertion permits display-off/dark wake while retaining acquisition.
    setProperty("SystemSuspendResumeQualified",false);
    if(pollingAssertion)IOLog("M106 power: polled-input CPU assertion=%llu; display-off allowed, system suspend unsupported\n",pollingAssertion);
    else if(ticks<6000)IOLog("M106 power: PM assertion pending root=%p tick=%u\n",sleepRoot,ticks);
}
void NativeSprdPlatform::identifyTouch() {
    uint8_t *id=goodixId,config[5]={};Result r=bus3.readRegister(0x14,0x8140,2,id,sizeof(goodixId));goodixResult=r;
    bool numeric=true;for(unsigned i=0;i<3;i++)numeric&=id[i]>='0' && id[i]<='9';
    IOLog("bbk touch: Goodix@14 status=%d id=%02x %02x %02x %02x version=%02x%02x\n",r,id[0],id[1],id[2],id[3],id[5],id[4]);
    if(r==Ok && numeric) {
        r=bus3.readRegister(0x14,0x8047,2,config,sizeof(config));
        goodixConfigResult=r;
        unsigned w=config[1]|((unsigned)config[2]<<8),h=config[3]|((unsigned)config[4]<<8);
        if(r==Ok && w>=64 && h>=64 && w<=4096 && h<=4096){touchKind=1;touchW=w;touchH=h;return;}
        IOLog("bbk touch: Goodix config invalid status=%d size=%ux%u\n",r,w,h);return;
    }
    uint8_t *fts=ftsId;Result a=bus3.readRegister(0x38,0xa3,1,&fts[0],1);
    Result b=bus3.readRegister(0x38,0xa8,1,&fts[1],1),c=bus3.readRegister(0x38,0xa6,1,&fts[2],1);
    ftsResult[0]=a;ftsResult[1]=b;ftsResult[2]=c;
    IOLog("bbk touch: FocalTech@38 status=%d/%d/%d chip=%02x vendor=%02x fw=%02x\n",a,b,c,fts[0],fts[1],fts[2]);
    // An address ACK or all-zero register is not a supported chip identity.
    // The board's FTS driver supports multiple IC generations; qualify one
    // from real ID evidence before enabling its generation-specific parser.
    setProperty("TouchControllerIdentified",false);
}
void NativeSprdPlatform::recoverTouchReset() {
    // Reset only the two factory touch candidates after both addresses NACK.
    // A responding controller keeps its firmware configuration and state.
    if(goodixResult!=Nack || ftsResult[0]!=Nack || ftsResult[1]!=Nack || ftsResult[2]!=Nack)return;
    IODeviceMemory *memory=IODeviceMemory::withRange(0x32450000,0x1000);
    touchPinMap=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    OSSafeReleaseNULL(memory);
    if(!touchPinMap){IOLog("M105 touch: pinctrl map failed\n");return;}
    touchPinRegisters.base=(volatile uint8_t *)touchPinMap->getVirtualAddress();
    IOLog("M105 touch: before reset gpio=0x%x dir=0x%x inen=0x%x pins=%x/%x/%x/%x\n",
        mmio[1].read32(0x480),mmio[1].read32(0x488),mmio[1].read32(0x4a8),
        touchPinRegisters.read32(0x2b8),touchPinRegisters.read32(0x2bc),touchPinRegisters.read32(0x2c0),touchPinRegisters.read32(0x2c4));
    Result r=touchPins.resetToAddress14();
    IOLog("M105 touch: factory reset result=%d gpio=0x%x\n",r,mmio[1].read32(0x480));
    if(r==Ok)identifyTouch();
}
void NativeSprdPlatform::readTouch() {
    if(!touch || touchKind!=1)return;
    uint8_t status=0,raw[80]={};Result r=bus3.readRegister(0x14,0x814e,2,&status,1);
    if(r!=Ok){if(touchErrors++<8)IOLog("bbk touch: status read failed %d\n",r);return;}
    if(!(status&0x80))return;
    Frame frame={};unsigned count=status&15;
    if(count<=10 && count)r=bus3.readRegister(0x14,0x814f,2,raw,count*8);
    if(r==Ok)r=goodixFrame(raw,sizeof(raw),count,touchW,touchH,frame);
    const uint8_t clear=0;
    if(r!=Ok){bus3.writeRegister(0x14,0x814e,2,&clear,1);if(touchErrors++<8)IOLog("bbk touch: invalid frame=%d status=0x%x\n",r,status);return;}
    IOReturn sent=sprd_hid_contacts(touch,&frame,touchW,touchH);
    // Keep a valid hardware sample pending until the real consumer accepts
    // it. In particular do not discard reset's measured zero-contact state
    // while IOHIDEventDriver is still opening its interface.
    if(sent!=kIOReturnSuccess){if(touchErrors++<8)IOLog("M107 touch: report pending hid=0x%x contacts=%u\n",sent,frame.count);return;}
    Result ack=bus3.writeRegister(0x14,0x814e,2,&clear,1);
    if(ack!=Ok){if(touchErrors++<8)IOLog("bbk touch: ack error=%d status=0x%x\n",ack,status);return;}
    if(frame.count!=lastFrame.count || touchReports<6)IOLog("bbk touch: frame contacts=%u first=%u,%u hid=0x%x\n",frame.count,frame.contacts[0].x,frame.contacts[0].y,sent);
    lastFrame=frame;touchReports++;
}
void NativeSprdPlatform::poll() {
    requirePollingPower();
    uint16_t raw=0;Result r=adi.read(0x280,raw);
    if(r==Ok) {
        uint32_t gpio=mmio[1].read32(7*0x80);
        uint32_t mask=(!(raw&(1u<<1))?2:0)|((raw&(1u<<4))?4:0)|(!(gpio&(1u<<12))?8:0);
        debounce.sample(mask);IOReturn sent=keys?sprd_hid_buttons(keys,(uint8_t)debounce.stable):kIOReturnNotReady;
        if(mask!=lastPhysicalMask || ticks<3 || !(ticks%1000)) {
            IOLog("M102 keys: tick=%u eic=0x%x gpio124=%u mask=0x%x stable=0x%x hid=0x%x\n",ticks,raw,(gpio>>12)&1,mask,debounce.stable,sent);
            lastPhysicalMask=mask;
        }
    } else if(keyErrors++<8)IOLog("bbk input: PMIC read failed %d; no fabricated key state\n",r);
    // Print remembered probe results after the early console transition too.
    // This records observed IDs; it does not reinitialize or reset devices.
    if(!(ticks%1000) && (ticks<6000 || !(ticks%6000))) {
        IOLog("M102 platform: tick=%u pmic=%04x/%04x goodix=%d config=%d id=%02x%02x%02x%02x kind=%u size=%ux%u fts=%d/%d/%d id=%02x/%02x/%02x charger=%d/%02x\n",
            ticks,chipLo,chipHi,goodixResult,goodixConfigResult,goodixId[0],goodixId[1],goodixId[2],goodixId[3],touchKind,touchW,touchH,
            ftsResult[0],ftsResult[1],ftsResult[2],ftsId[0],ftsId[1],ftsId[2],chargerResult,chargerId);
        for(unsigned i=0;i<2;i++)IOLog("M102 i2c%u: ctl=0x%x status=0x%x count=0x%x\n",i+3,mmio[i+4].read32(0),mmio[i+4].read32(0x14),mmio[i+4].read32(8));
        if(keys)sprd_hid_diagnostics(keys);if(touch)sprd_hid_diagnostics(touch);
    }
    if(!(ticks%2))readTouch();
    if(!(ticks%6000)) {
        uint16_t voltage=0,current=0,area=0,module=0,clock=0;
        Result v=adi.read(0xc20,voltage),c=adi.read(0xc2c,current),a=adi.read(0xca8,area);
        IOLog("bbk battery: raw voltage=%u current=%u user=0x%x status=%d/%d/%d (SOC not initialized, not published as percent)\n",voltage,current,area,v,c,a);
        Result m=adi.read(0x1808,module),t=adi.read(0x1810,clock);int mv=0,ma=0;
        Result decoded=(fuelCalibrationResult==Ok && v==Ok && c==Ok && m==Ok && t==Ok && (module&0x80) && (clock&0x40))?
            fuelMeasurements(voltage,current,fuelCal,mv,ma):Invalid;
        IOLog("M106 fuel: sample status=%d voltage_mV=%d current_mA=%d module=0x%x rtc=0x%x persisted_capacity_present=%u\n",
            decoded,mv,ma,module,clock,a==Ok && (area&0xfff)<=1000);
        if(decoded==Ok){setProperty("FGUVoltageMillivolts",(unsigned)mv,32);setProperty("FGUCurrentMilliamps",(unsigned)ma,32);}
        uint8_t charge=0,fault=0;Result cr=bus4.readRegister(0x6b,8,1,&charge,1),fr=bus4.readRegister(0x6b,9,1,&fault,1);
        IOLog("M106 charger: status=%d/%d raw=0x%x fault=0x%x (read only)\n",cr,fr,charge,fault);
    }
    ticks++;if(timer)timer->setTimeoutMS(10);
}
bool NativeSprdPlatform::start(IOService *p) {
    if(!IOService::start(p))return false;
    loop=IOWorkLoop::workLoop();if(!loop){stop(p);return false;}
    for(unsigned i=0;i<7;i++){maps[i]=p->mapDeviceMemoryWithIndex(i);if(!maps[i]){IOLog("bbk platform: MMIO map failed index=%u\n",i);stop(p);return false;}mmio[i].base=(volatile uint8_t *)maps[i]->getVirtualAddress();}
    if(!prepare()){IOLog("bbk platform: hardware prepare failed\n");stop(p);return false;}
    prepareFuelGauge();
    identifyTouch();recoverTouchReset();
    if(!publishInput()){IOLog("bbk platform: HID publication failed\n");stop(p);return false;}
    timer=IOTimerEventSource::timerEventSource(this,tick);
    if(!timer || loop->addEventSource(timer)!=kIOReturnSuccess){stop(p);return false;}
    registerService();poll();
    backlight=sprdBacklightCreate(this);
    IOLog("M104 backlight: service=%p; failure does not change existing framebuffer ownership\n",backlight);
    IOLog("bbk platform: ready ADI/GPIO/I2C3/4, keys polled at 10ms\n");return true;
}
void NativeSprdPlatform::restore() {
    OSSafeReleaseNULL(fuelLockMap);fuelLockRegisters.base=nullptr;
    if(touchPinMap){touchPins.restore();OSSafeReleaseNULL(touchPinMap);touchPinRegisters.base=nullptr;}
    if(pmicChanged && mmio[0].base)adi.update(0x284,0x1a,pmicMask);pmicChanged=false;
    if(gpioChanged)for(unsigned i=0;i<3;i++){unsigned off=7*0x80+(i==0?4:i==1?8:0x28);mmio[1].write32(off,(mmio[1].read32(off)&~(1u<<12))|(gpioSaved[i]&(1u<<12)));}gpioChanged=false;
    if(clockChanged)for(unsigned i=0;i<2;i++){mmio[4+i].write32(0,0);mmio[6].write32(0x40+4*i,clockSaved[i]);}clockChanged=false;
    if(mmio[3].base && apAdded)mmio[3].write32(0x2000,apAdded);apAdded=0;
    if(mmio[2].base)for(unsigned i=0;i<2;i++)if(aonAdded[i])mmio[2].write32(0x2000+(i?8:4),aonAdded[i]);
    aonAdded[0]=aonAdded[1]=0;
}
void NativeSprdPlatform::stop(IOService *p) {
    if(backlight){backlight->stop(this);backlight->detach(this);backlight->release();backlight=nullptr;}
    if(timer){timer->cancelTimeout();if(loop)loop->removeEventSource(timer);OSSafeReleaseNULL(timer);}
    if(pollingAssertion && sleepRoot){sleepRoot->releasePMAssertion(pollingAssertion);pollingAssertion=kIOPMUndefinedDriverAssertionID;sleepRoot=nullptr;}
    IOService **devices[]={&touch,&keys};bool started[]={touchStarted,keysStarted};
    for(unsigned i=0;i<2;i++){IOService **d=devices[i];if(*d){if((*d)->getProvider()==this){if(started[i])(*d)->stop(this);(*d)->detach(this);}(*d)->release();*d=nullptr;}}
    touchStarted=keysStarted=false;
    if(touchPersonality){gIOCatalogue->removeDrivers(touchPersonality,false);OSSafeReleaseNULL(touchPersonality);}
    restore();for(auto &m:maps)OSSafeReleaseNULL(m);OSSafeReleaseNULL(loop);IOService::stop(p);
}
