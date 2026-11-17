// SPDX-License-Identifier: GPL-2.0-only
// EEBBK 1h190 hardware acquisition. No Apple register addresses or emulated
// battery values. Register protocol provenance is recorded in README.md.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
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
#include <pexpert/pexpert.h>
#include <new>
#include "SprdBus.h"
#include "SprdBacklight.h"
#include "SprdTouchPins.h"
#include "SprdFuelGauge.h"
#include "SprdBattery.h"
#include "SprdSdio.h"
#include "SprdSdioDma.h"
#include "SprdWcnPackets.h"
#include "SprdWcnCalibration.h"
#include "SprdWcnBluetooth.h"
#include "SprdWcnBluetoothData.h"
#include "SprdWcnApi.h"
#include "SprdWcnSession.h"
#include "SprdWcnPower.h"
#include "SprdWcnScan.h"
#include "SprdMali.h"
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void sprd_hid_static_init() __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *sprd_hid_allocate(bool);
extern "C" IOReturn sprd_hid_buttons(IOService *,uint8_t);
extern "C" IOReturn sprd_hid_contacts(IOService *,const SprdBus::Frame *,unsigned,unsigned);
extern "C" void sprd_hid_diagnostics(IOService *);
extern "C" IOReturn sprd_mali_owned_write_test(volatile uint8_t *,volatile uint8_t *,volatile uint8_t *,volatile uint8_t *,bool *);
extern "C" const uint8_t sprd_wcn_firmware_start[];
extern "C" const uint8_t sprd_wcn_firmware_end[];
using namespace SprdBus;
namespace {
constexpr FuelOcvPoint kFactoryOcvCapacity[]={{4386000,100},{4329000,95},{4274000,90},{4217000,85},{4160000,80},{4106000,75},{4055000,70},{4008000,65},{3964000,60},{3905000,55},{3866000,50},{3840000,45},{3818000,40},{3800000,35},{3784000,30},{3766000,25},{3744000,20},{3719000,15},{3690000,10},{3679000,5},{3460000,0}};
struct Registers {
    volatile uint8_t *base=nullptr;
    uint32_t read32(unsigned off) {uint32_t v=*(volatile uint32_t *)(base+off);__asm__ volatile("dsb ld":::"memory");return v;}
    void write32(unsigned off,uint32_t v) {__asm__ volatile("dsb st":::"memory");*(volatile uint32_t *)(base+off)=v;__asm__ volatile("dsb sy":::"memory");}
    uint8_t read8(unsigned off) {uint8_t v=base[off];__asm__ volatile("dsb ld":::"memory");return v;}
    void write8(unsigned off,uint8_t v) {__asm__ volatile("dsb st":::"memory");base[off]=v;}
    uint16_t read16(unsigned off) {uint16_t v=*(volatile uint16_t *)(base+off);__asm__ volatile("dsb ld":::"memory");return v;}
    void write16(unsigned off,uint16_t v) {__asm__ volatile("dsb st":::"memory");*(volatile uint16_t *)(base+off)=v;__asm__ volatile("dsb sy":::"memory");}
    void delay(unsigned us) {IODelay(us);}
};
constexpr uint32_t kWcnFirmwareBase=0x40500000;
constexpr uint32_t kWcnResetRegister=0x40088288;
constexpr uint32_t kWcnMailbox=0x405e73b0;
constexpr size_t kWcnFirmwareBytes=0xe7400;
constexpr uint32_t kWcnMailboxWait=0xf0f0f0f1;
constexpr uint32_t kWcnMailboxCalibrated=0xf0f0f0f2;
constexpr uint32_t kWcnMailboxReady=0xf0f0f0ff;

Result bootWcnFirmware(SprdSdio::Host<Registers> &host,const SprdSdio::Identity &id) {
    const size_t firmwareBytes=static_cast<size_t>(sprd_wcn_firmware_end-sprd_wcn_firmware_start);
    if(id.chip!=0x2355b001 || id.strap!=0 || firmwareBytes!=kWcnFirmwareBytes)
        return Invalid;
    uint32_t vectorSp=0,vectorReset=0,reset=0;
    memcpy(&vectorSp,sprd_wcn_firmware_start,sizeof(vectorSp));
    memcpy(&vectorReset,sprd_wcn_firmware_start+4,sizeof(vectorReset));
    if(vectorSp!=0x001a0aa0 || vectorReset!=0x00100319)return Protocol;
    Result result=host.readCp(kWcnResetRegister,reset);
    if(result!=Ok)return result;
    uint32_t held=reset|1u;
    result=host.writeCpBytes(kWcnResetRegister,reinterpret_cast<const uint8_t *>(&held),sizeof(held));
    if(result!=Ok)return result;
    IOLog("M121 wcn: CPU held reset=0x%x firmware=0x%llx bytes=0x%llx\n",reset,static_cast<unsigned long long>(kWcnFirmwareBase),static_cast<unsigned long long>(firmwareBytes));
    result=host.writeCpBytes(kWcnFirmwareBase,sprd_wcn_firmware_start,firmwareBytes);
    if(result!=Ok)return result;
    uint8_t vectors[8]={};result=host.readCpBytes(kWcnFirmwareBase,vectors,sizeof(vectors));
    if(result!=Ok || memcmp(vectors,sprd_wcn_firmware_start,sizeof(vectors)))return result==Ok?Protocol:result;
    IOLog("M121 wcn: firmware written/readback vector sp=0x%08x reset=0x%08x bytes=0x%llx\n",vectorSp,vectorReset,static_cast<unsigned long long>(firmwareBytes));
    result=host.readCp(kWcnResetRegister,reset);if(result!=Ok)return result;
    uint32_t released=reset&~1u;
    result=host.writeCpBytes(kWcnResetRegister,reinterpret_cast<const uint8_t *>(&released),sizeof(released));
    if(result!=Ok)return result;
    uint32_t mailbox=0;bool calibratedRequest=false;
    for(unsigned i=0;i<66;i++){
        result=host.readCp(kWcnMailbox,mailbox);if(result!=Ok)return result;
        if(mailbox==kWcnMailboxWait){calibratedRequest=true;break;}
        IODelay(5000);
    }
    if(!calibratedRequest){IOLog("M121 wcn: calibration request timeout mailbox=0x%08x\n",mailbox);return Timeout;}
    // The factory loader returns 0xffff when the optional productinfo TSX file
    // or its AFC calibration flag is absent; preserve that exact fallback.
    const uint16_t tsxDac=0xffff;
    result=host.writeCpBytes(kWcnMailbox+0x18,reinterpret_cast<const uint8_t *>(&tsxDac),sizeof(tsxDac));
    if(result!=Ok)return result;
    result=host.writeCpBytes(kWcnMailbox,reinterpret_cast<const uint8_t *>(&kWcnMailboxCalibrated),sizeof(kWcnMailboxCalibrated));
    if(result!=Ok)return result;
    bool ready=false;
    for(unsigned i=0;i<66;i++){
        result=host.readCp(kWcnMailbox,mailbox);if(result!=Ok)return result;
        if(mailbox==kWcnMailboxReady){ready=true;break;}
        IODelay(5000);
    }
    IOLog("M121 wcn: mailbox wait=0x%08x tsx=0x%04x result=%d mailbox=0x%08x\n",kWcnMailboxWait,tsxDac,ready?Ok:Timeout,mailbox);
    return ready?Ok:Timeout;
}

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
    FuelCounterState fuelCounter;NativeSprdBattery *battery=nullptr;
    const char *capacityOrigin=nullptr;unsigned batteryFailures=0;
    bool batteryStarted=false,batteryRemovalSeen=false;
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
    bool prepare();bool publishInput();void identifyTouch();void recoverTouchReset();void prepareFuelGauge();void readBattery();void requirePollingPower();void poll();void readTouch();void restore();void identifyRadioGpu();
public:
    IOService *probe(IOService *p,SInt32 *score) override {if(!compatible(p))return nullptr;if(score)*score=2200;return this;}
    IOWorkLoop *getWorkLoop() const override {return loop;}
    bool start(IOService *) override;void stop(IOService *) override;
};
OSDefineMetaClassAndStructors(NativeSprdPlatform,IOService)

void NativeSprdPlatform::identifyRadioGpu() {
    // Run before this service starts its polling timer. ADI transactions share
    // the platform's existing controller owner; no second ADI worker is added.
    IOMemoryMap *m[5]={};Registers r[5];
    const uint64_t physical[5]={0x71200000,0x32080000,0x327e0000,0x60100000,0x60000000};
    bool mapped=true;
    for(unsigned i=0;i<5;i++){
        IODeviceMemory *d=IODeviceMemory::withRange(physical[i],i==4?0x100000:0x1000);
        m[i]=d?d->map(kIOMapInhibitCache):nullptr;OSSafeReleaseNULL(d);
        if(!m[i]){mapped=false;IOLog("M116 peripheral: map failed index=%u physical=0x%llx\n",i,(unsigned long long)physical[i]);break;}
        r[i].base=(volatile uint8_t *)m[i]->getVirtualAddress();
    }
    if(mapped){
        SprdWcn::Power<Registers,Adi<Registers>> power(mmio[1],mmio[2],mmio[3],r[1],mmio[6],adi);
        Result p=power.enable();IOLog("M116 wcn: shared power=%d host=0x71200000 aux0=0x%x apclk=0x%x\n",p,r[1].read32(0x228),mmio[6].read32(0x88));
        IOLog("M117 wcn: stage=%u reg=0x%x mask=0x%x want=0x%x got=0x%x result=%d protection=0x%x->0x%x\n",power.stage,power.lastReg,power.lastMask,power.lastWant,power.lastGot,power.lastResult,power.protectionBefore,power.protectionAfter);
        IOLog("M118 wcn: gpio=%u high=%u data=0x%x dir=0x%x inen=0x%x dmsk=0x%x\n",power.lastPin,power.lastHigh,power.lastData,power.lastDirection,power.lastInput,power.lastDataMask);
        SprdSdioDma dma;bool dmaReady=p==Ok && dma.initialize();
        SprdSdio::Host<Registers> host(r[0],dma);SprdSdio::Identity id;
        Result h=dmaReady?host.initialize():Invalid,e=h==Ok?host.enumerate(id):Invalid;
        IOLog("M116 wcn: host=%d enumerate=%d ocr=0x%x funcs=%u rca=0x%x cccr=0x%x ready=0x%x manfid=0x%x/0x%x chip=0x%x strap=0x%x\n",h,e,id.ocr,id.functions,id.rca,id.cccr,id.ready,id.vendor,id.device,id.chip,id.strap);
        IOLog("M116 wcn: command=%u arg=0x%x status=0x%x present=0x%x resp=0x%x copied=%llu\n",host.last.index,host.last.argument,host.last.status,host.last.present,host.last.response,(unsigned long long)host.last.transferred);
        if(h==Ok && e==Ok){
            SprdWcn::Session<SprdSdio::Host<Registers>> session(host);
            uint8_t *btSnapshot=nullptr;
            Result fw=session.enable();
            uint32_t fastClock=0;
            if(fw==Ok && PE_parse_boot_argn("bbk-wcn-fast-clock",&fastClock,sizeof(fastClock)) && fastClock==1){
                fw=host.clock(0x0400);uint32_t check=0;
                if(fw==Ok)fw=host.readCp(0x4083c208,check);
                if(fw==Ok && check!=id.chip)fw=Protocol;
                IOLog("M123 wcn: fast clock=0x0400 qualification=%d chip=0x%x\n",fw,check);
            }
            if(fw==Ok)fw=bootWcnFirmware(host,id);
            IOLog("M121 wcn: firmware boot result=%d\n",fw);
            if(fw==Ok){
                fw=session.query();
                IOLog("M123 wcn: session query=%d stage=%u pub=0x%x sleep=0x%x wifi=0x%x+0x%x bt=0x%x+0x%x\n",fw,session.stage,session.lastPublic,session.lastSleep,session.wifi.address,session.wifi.bytes,session.bt.address,session.bt.bytes);
                uint8_t *wifiSnapshot=nullptr;
                if(fw==Ok){
                    wifiSnapshot=(uint8_t *)IOMalloc(session.wifi.bytes);btSnapshot=(uint8_t *)IOMalloc(session.bt.bytes);
                    if(!wifiSnapshot || !btSnapshot)fw=Invalid;
                    else{
                        IOLog("M123 wcn: snapshot and restore Wi-Fi memory begin\n");
                        fw=session.saveAndOpenWifi(wifiSnapshot,session.wifi.bytes,btSnapshot,session.bt.bytes);
                    }
                }
                IOLog("M123 wcn: session open=%d stage=%u pub=0x%x sleep=0x%x cmd=%u arg=0x%x status=0x%x ps=0x%x resp=0x%x\n",fw,session.stage,session.lastPublic,session.lastSleep,host.last.index,host.last.argument,host.last.status,host.last.present,host.last.response);
                if(wifiSnapshot)IOFree(wifiSnapshot,session.wifi.bytes);
            }
            if(fw==Ok){
                const unsigned bank=6*0x80;uint32_t mask=mmio[1].read32(bank+4),dir=mmio[1].read32(bank+8),in=mmio[1].read32(bank+0x28);
                mmio[1].write32(bank+4,mask|1u);mmio[1].write32(bank+8,dir&~1u);mmio[1].write32(bank+0x28,in|1u);
                uint8_t irqEnable=0;Result packet=host.byte(0,0x1ca,irqEnable);bool irqSaved=packet==Ok;
                if(packet==Ok)packet=host.put(0,0x1ca,(uint8_t)(irqEnable|3));
                IOLog("M122 wcn: packet setup=%d deint=0x%x gpio96=%u rx_base=12\n",packet,irqEnable,(mmio[1].read32(bank)&1u)!=0);
                uint8_t api[260]={};SprdWcn::put32(api,1);memcpy(api+4,SprdWcn::kFactoryWifiApi,256);
                SprdWcn::WifiReply sync;
                if(packet==Ok)packet=SprdWcn::wifiCommand(host,mmio[1],9,1,api,sizeof(api),sync);
                IOLog("M122 wifi: sync result=%d status=%d bytes=%llu frames=%u unmatched=%u header=0x%x valid=%u remain=%u cmd=%u arg=0x%x irq=%u\n",packet,sync.status,(unsigned long long)sync.length,sync.frames,sync.unmatched,sync.publicHeader,sync.validBytes,sync.remainingBytes,host.last.index,host.last.argument,(mmio[1].read32(bank)&1u)!=0);
                IOLog("M124 wifi: ADMA RX=2520 head=%08x/%08x/%08x/%08x tail=%08x/%08x/%08x/%08x sd_status=0x%x response=0x%x copied=%llu\n",sync.firstWords[0],sync.firstWords[1],sync.firstWords[2],sync.firstWords[3],sync.trailer[0],sync.trailer[1],sync.trailer[2],sync.trailer[3],host.last.status,host.last.response,(unsigned long long)host.last.transferred);
                uint32_t transaction=2;
                for(const auto &section:SprdWcn::kFactoryWifiSections){
                    if(packet!=Ok)break;
                    uint8_t body[1470]={};size_t length=0;SprdWcn::WifiReply reply;
                    packet=SprdWcn::encodeWifiIni(body,sizeof(body),section,length);
                    if(packet==Ok)packet=SprdWcn::wifiCommand(host,mmio[1],76,transaction++,body,length,reply);
                    IOLog("M125 wifi: INI section=%u bytes=%llu crc=0x%04x result=%d status=%d frames=%u unmatched=%u\n",section.index,(unsigned long long)section.bytes,SprdWcn::wifiCalibrationCrc(section.data,section.bytes),packet,reply.status,reply.frames,reply.unmatched);
                }
                if(packet==Ok){
                    SprdWcn::WifiReply info;packet=SprdWcn::wifiCommand(host,mmio[1],1,transaction++,nullptr,0,info);
                    if(packet==Ok && info.length<24)packet=Protocol;
                    IOLog("M122 wifi: GET_INFO result=%d status=%d bytes=%llu chip=0x%x revision=0x%x firmware=0x%x standards=0x%x capabilities=0x%x frames=%u unmatched=%u\n",packet,info.status,(unsigned long long)info.length,SprdWcn::le32(info.payload),SprdWcn::le32(info.payload+4),SprdWcn::le32(info.payload+8),SprdWcn::le32(info.payload+12),SprdWcn::le32(info.payload+16),info.frames,info.unmatched);
                }
                if(packet==Ok){
                    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
                    OSData *factoryMac=chosen?OSDynamicCast(OSData,chosen->getProperty("mac-address-wifi0")):nullptr;
                    uint8_t body[8]={};
                    packet=SprdWcn::encodeWifiOpen(body,sizeof(body),factoryMac?(const uint8_t *)factoryMac->getBytesNoCopy():nullptr,factoryMac?factoryMac->getLength():0);
                    const bool identityValid=packet==Ok;OSSafeReleaseNULL(chosen);
                    SprdWcn::WifiReply opened;
                    if(packet==Ok)packet=SprdWcn::wifiCommand(host,mmio[1],3,transaction++,body,sizeof(body),opened);
                    if(packet==Ok && opened.length!=0)packet=Protocol;
                    IOLog("M127 wifi: OPEN result=%d status=%d bytes=%llu ctx=%u factory_mac=%u\n",packet,opened.status,(unsigned long long)opened.length,opened.context,identityValid);
                    if(packet==Ok){
                        void *scanMemory=IOMalloc(sizeof(SprdWcn::WifiScanReport));
                        auto *scan=scanMemory?new(scanMemory) SprdWcn::WifiScanReport{}:nullptr;
                        if(scan){
                            Result scanned=SprdWcn::wifiScan(host,mmio[1],opened.context,transaction++,0x421,*scan);
                            IOLog("M128 wifi: scan result=%d response=%u status=%d code=0x%x done=%u done_type=%u events=%u aggregates=%u frames=%u bss=%u dropped=%u invalid=%u channels=0x421\n",
                                  scanned,(unsigned)scan->responseSeen,scan->responseStatus,scan->responseCode,(unsigned)scan->doneSeen,scan->doneType,
                                  scan->events,scan->aggregates,scan->scanFrames,scan->uniqueNetworks,scan->droppedNetworks,scan->invalidFrames);
                            scan->~WifiScanReport();
                            IOFree(scanMemory,sizeof(*scan));
                        }else IOLog("M128 wifi: scan allocation failed\n");
                        uint8_t mode=1;SprdWcn::WifiReply closed;
                        packet=SprdWcn::wifiCommand(host,mmio[1],4,transaction++,&mode,sizeof(mode),closed,opened.context);
                        IOLog("M127 wifi: CLOSE result=%d status=%d bytes=%llu ctx=%u\n",packet,closed.status,(unsigned long long)closed.length,opened.context);
                    }
                }
                if(packet==Ok && btSnapshot){
                    Result bluetooth=session.openBluetooth(btSnapshot,session.bt.bytes);
                    IOLog("M126 bt: memory open=%d stage=%u public=0x%x sleep=0x%x\n",bluetooth,session.stage,session.lastPublic,session.lastSleep);
                    uint8_t pskey[160]={};memcpy(pskey,SprdWcn::kFactoryBtPskey,sizeof(pskey));
                    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
                    OSData *factoryMac=chosen?OSDynamicCast(OSData,chosen->getProperty("mac-address-bluetooth0")):nullptr;
                    bool identityValid=factoryMac && factoryMac->getLength()==6;
                    if(identityValid){
                        const uint8_t *mac=(const uint8_t *)factoryMac->getBytesNoCopy();uint8_t any=0;
                        for(unsigned i=0;i<6;i++){any|=mac[i];pskey[20+i]=mac[5-i];}
                        identityValid=any && !(mac[0]&1);
                    }
                    OSSafeReleaseNULL(chosen);
                    if(bluetooth==Ok && !identityValid)bluetooth=Invalid;
                    const uint8_t enable[]={0,9,1};
                    const uint16_t opcode[]={0xfca0,0xfca2,0xfca1,0x0c03};
                    const uint8_t *parameters[]={pskey,SprdWcn::kFactoryBtRf,enable,nullptr};
                    const size_t lengths[]={sizeof(pskey),sizeof(SprdWcn::kFactoryBtRf),sizeof(enable),0};
                    bool enabled=false;
                    for(unsigned step=0;step<4 && bluetooth==Ok;step++){
                        SprdWcn::HciReply reply;
                        bluetooth=SprdWcn::hciCommand(host,mmio[1],opcode[step],parameters[step],lengths[step],reply);
                        if(step==2 && bluetooth==Ok)enabled=true;
                        IOLog("M126 bt: init opcode=0x%04x bytes=%llu result=%d status=0x%x complete=%u events=%u unmatched=%u aggregates=%u header=0x%x valid=%u remain=%u\n",opcode[step],(unsigned long long)lengths[step],bluetooth,reply.status,reply.complete,reply.events,reply.unmatched,reply.aggregates,reply.publicHeader,reply.validBytes,reply.remainingBytes);
                    }
                    if(bluetooth==Ok){
                        SprdWcn::HciReply reply;bluetooth=SprdWcn::hciCommand(host,mmio[1],0x1001,nullptr,0,reply);
                        if(bluetooth==Ok && reply.length!=8)bluetooth=Protocol;
                        IOLog("M126 bt: version result=%d status=0x%x bytes=%llu hci=0x%x revision=0x%x lmp=0x%x manufacturer=0x%x subversion=0x%x\n",bluetooth,reply.status,(unsigned long long)reply.length,reply.payload[0],SprdWcn::le16(reply.payload+1),reply.payload[3],SprdWcn::le16(reply.payload+4),SprdWcn::le16(reply.payload+6));
                    }
                    if(bluetooth==Ok){
                        SprdWcn::HciReply reply;bluetooth=SprdWcn::hciCommand(host,mmio[1],0x1009,nullptr,0,reply);
                        bool addressMatch=bluetooth==Ok && reply.length==6 && !memcmp(reply.payload,pskey+20,6);
                        if(bluetooth==Ok && !addressMatch)bluetooth=Protocol;
                        IOLog("M126 bt: factory address match=%u result=%d status=0x%x bytes=%llu\n",addressMatch,bluetooth,reply.status,(unsigned long long)reply.length);
                    }
                    if(enabled){
                        const uint8_t disable[]={0,9,0};SprdWcn::HciReply reply;
                        Result disabled=SprdWcn::hciCommand(host,mmio[1],0xfca1,disable,sizeof(disable),reply);
                        IOLog("M126 bt: disable result=%d status=0x%x\n",disabled,reply.status);
                        if(bluetooth==Ok)bluetooth=disabled;
                    }
                    IOLog("M126 bt: HCI transaction result=%d factory_identity=%u\n",bluetooth,identityValid);
                    if(packet==Ok)packet=bluetooth;
                }
                Result irqRestored=irqSaved?host.put(0,0x1ca,irqEnable):Invalid;
                mmio[1].write32(bank+0x28,in);mmio[1].write32(bank+8,dir);mmio[1].write32(bank+4,mask);
                IOLog("M122 wcn: packet cleanup irq=%d gpio_restored=1 final=%d\n",irqRestored,packet);
            }
            uint32_t reset=0,held=0;
            Result stopped=session.wake();
            if(stopped==Ok)stopped=host.readCp(kWcnResetRegister,reset);
            if(stopped==Ok){
                held=reset|1u;
                stopped=host.writeCpBytes(kWcnResetRegister,reinterpret_cast<const uint8_t *>(&held),sizeof(held));
            }
            if(stopped==Ok)stopped=host.readCp(kWcnResetRegister,held);
            IOLog("M121 wcn: firmware test quiesce result=%d reset=0x%x held=%u\n",stopped,held,(held&1)!=0);
            Result publicRestored=session.restore();
            IOLog("M123 wcn: public interrupt enable restored=%d\n",publicRestored);
            if(btSnapshot)IOFree(btSnapshot,session.bt.bytes);
        }
        host.restore();Result restored=power.restore();IOLog("M116 wcn: power restored=%d; firmware test ended, Wi-Fi/BT frontend not yet published\n",restored);
        uint16_t pd=0,vsel=0;Result a=adi.read(0x181c,pd),b=adi.read(0x1854,vsel);
        IOLog("M116 mali: DCDC_GPU reads=%d/%d pd=0x%x selector=%u (3125uV/step)\n",a,b,pd,vsel&0x1ff);
        if(a==Ok && b==Ok && SprdMali::factorySupplyReady(pd,vsel)){
            SprdMali::Probe<Registers> gpu(r[2],mmio[2],r[3],r[4]);SprdMali::Identity g;
            Result gr=gpu.run(g);
            IOLog("M116 mali: result=%d id=0x%x status=0x%x shader=0x%x tiler=0x%x l2=0x%x as=0x%x js=0x%x irq=0x%x reset_polls=%u\n",gr,g.id,g.status,g.shader,g.tiler,g.l2,g.as,g.js,g.irq,g.resetPolls);
            bool canRestore=true;
            if(gr==Ok){
                IOReturn job=sprd_mali_owned_write_test(r[4].base,r[2].base,mmio[2].base,r[3].base,&canRestore);
                IOLog("M119 mali: owned-memory job=0x%x domain_restorable=%u\n",job,canRestore);
            }
            if(canRestore){gpu.restore();IOLog("M116 mali: clock/domain restored; no Metal device published by this identification operation\n");}
            else IOLog("M119 mali: domain retained and GPU buffers quarantined after unconfirmed DMA stop\n");
        }else IOLog("M116 mali: stopped before GPU register access: factory always-on supply is not qualified\n");
    }
    for(auto &v:m)OSSafeReleaseNULL(v);
}

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
        touchPersonality=OSDictionary::withCapacity(9);
        OSArray *drivers=OSArray::withCapacity(1);OSNumber *vendor=OSNumber::withNumber(0x1782ULL,32);
        OSNumber *product=OSNumber::withNumber(0x1902ULL,32),*score=OSNumber::withNumber(100000ULL,32);
        bool ok=touchPersonality && drivers && vendor && product && score;
        // IOHIDInterface2031 delegates family matching to IOHIDDevice when
        // MultipleInterfaceEnabled is absent. VendorID/ProductID are already
        // on that physical device, but its default newVendorIDNumber() is
        // NULL, so an IOPropertyMatch on the child interface cannot succeed.
        // Use the original HID family's top-level keys and score instead.
        if(ok)ok=touchPersonality->setObject("VendorID",vendor) && touchPersonality->setObject("ProductID",product) &&
            stringProperty(touchPersonality,"CFBundleIdentifier","com.apple.iokit.IOHIDFamily") &&
            stringProperty(touchPersonality,"IOClass","IOHIDEventDriver") &&
            stringProperty(touchPersonality,"IOProviderClass","IOHIDInterface") &&
            stringProperty(touchPersonality,"IOMatchCategory","IODefaultMatchCategory") &&
            touchPersonality->setObject("IOProbeScore",score) &&
            touchPersonality->setObject("NativeSprdPhysicalTouch",kOSBooleanTrue) && drivers->setObject(touchPersonality);
        if(ok)ok=gIOCatalogue->addDrivers(drivers,true);
        OSSafeReleaseNULL(drivers);OSSafeReleaseNULL(vendor);OSSafeReleaseNULL(product);OSSafeReleaseNULL(score);
        IOLog("M109 touch: original HID family matching installed=%u vendor=1782 product=1902\n",ok);
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
void NativeSprdPlatform::readBattery() {
    uint16_t voltage=0,current=0,module=0,clock=0,eic=0,config=0;
    uint32_t counter=0;int mv=0,ma=0,capacity=-1;
    Result result=fuelCalibrationResult;
    if(result==Ok)result=adi.read(0x1808,module);
    if(result==Ok)result=adi.read(0x1810,clock);
    if(result==Ok && (!(module&0x80) || !(clock&0x40)))result=Invalid;
    if(result==Ok)result=adi.read(0xc20,voltage);
    if(result==Ok)result=adi.read(0xc2c,current);
    if(result==Ok)result=fuelMeasurements(voltage,current,fuelCal,mv,ma);
    if(result==Ok && (mv<2500 || mv>5000))result=Invalid;
    if(result==Ok)result=adi.read(0x280,eic);
    if(result==Ok && !(eic&8)) {
        if(battery)battery->removed();
        batteryRemovalSeen=true;fuelCounter.initialized=false;return;
    }
    if(result==Ok && batteryRemovalSeen)result=Invalid; // hot-plug needs a new qualified boot OCV
    if(result==Ok)result=adi.read(0xc04,config);
    if(result==Ok)result=fuelCounterRead(adi,counter);
    uint64_t now=0;absolutetime_to_nanoseconds(mach_absolute_time(),&now);
    if(result==Ok && !fuelCounter.initialized) {
        uint16_t area=0,pocv=0,qmax=0;int ocv=0,openMa=0,percent=-1;
        result=adi.read(0xca8,area);
        if(result==Ok && fuelFirstPowerOn(area)) {
            result=adi.read(0xc28,pocv);
            if(result==Ok)result=adi.read(0xc74,qmax);
            if(result==Ok)result=fuelBootOcv(pocv,qmax,fuelCal,134000,ocv,openMa);
            if(result==Ok)result=fuelOcvCapacity(kFactoryOcvCapacity,sizeof(kFactoryOcvCapacity)/sizeof(kFactoryOcvCapacity[0]),(unsigned)ocv,percent);
            if(result==Ok){capacity=percent*10;capacityOrigin="FactoryBootOCV+CoulombCounter";}
        } else if(result==Ok) {
            result=fuelSavedDeciPercent(area,capacity);
            if(result==Ok)capacityOrigin="FactoryPersistedCapacity+CoulombCounter";
        }
        if(result==Ok)result=fuelCounter.begin(capacity,counter,now?now-1:0);
        IOLog("M114 fuel: initialize result=%d area=0x%x pocv=%u qmax=%u ocv_uV=%d open_mA=%d initial_deci=%d counter=0x%x cfg=0x%x eic=0x%x\n",
            result,area,pocv,qmax,ocv,openMa,capacity,counter,config,eic);
    }
    if(result==Ok)result=fuelCounter.observe(counter,now,fuelCal,capacity);
    uint8_t input=0,control=0,status=0,latched=0,live=0;
    if(result==Ok && (chargerResult!=Ok || chargerId!=0x2c))result=Invalid;
    if(result==Ok)result=bus4.readRegister(0x6b,0,1,&input,1);
    if(result==Ok)result=bus4.readRegister(0x6b,1,1,&control,1);
    if(result==Ok)result=bus4.readRegister(0x6b,8,1,&status,1);
    // BQ2560x REG09 first read is latched; the consecutive second read is
    // current state. Preserve both rather than inventing a healthy charger.
    if(result==Ok)result=bus4.readRegister(0x6b,9,1,&latched,1);
    if(result==Ok)result=bus4.readRegister(0x6b,9,1,&live,1);
    if(result!=Ok) {
        if(battery)battery->unavailable(result);
        if(batteryFailures++<8 || !(batteryFailures%12))IOLog("M114 battery: acquisition failed=%d module=0x%x rtc=0x%x eic=0x%x cfg=0x%x mv=%d ma=%d counter=0x%x; no status invented\n",
            result,module,clock,eic,config,mv,ma,counter);
        return;
    }
    if(!battery) {
        battery=OSTypeAlloc(NativeSprdBattery);
        if(!battery || !battery->init() || !battery->attach(this) || !(batteryStarted=battery->start(this))) {
            if(battery){if(battery->getProvider()==this)battery->detach(this);battery->release();battery=nullptr;}
            IOLog("M114 battery: IOPMPowerSource initialization failed\n");return;
        }
    }
    battery->acquired(capacity,mv,ma,true,chargerState(status,live,input,control),latched,live,
        counter,fuelCounter.accumulated,capacityOrigin);
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
    if(!(ticks%500))readBattery();
    if(!(ticks%6000)) {
        uint16_t voltage=0,current=0,area=0,module=0,clock=0,pocv=0,qmax=0;
        Result v=adi.read(0xc20,voltage),c=adi.read(0xc2c,current),a=adi.read(0xca8,area);
        IOLog("bbk battery: raw voltage=%u current=%u user=0x%x status=%d/%d/%d (SOC not initialized, not published as percent)\n",voltage,current,area,v,c,a);
        Result m=adi.read(0x1808,module),t=adi.read(0x1810,clock);int mv=0,ma=0;
        Result decoded=(fuelCalibrationResult==Ok && v==Ok && c==Ok && m==Ok && t==Ok && (module&0x80) && (clock&0x40))?
            fuelMeasurements(voltage,current,fuelCal,mv,ma):Invalid;
        IOLog("M106 fuel: sample status=%d voltage_mV=%d current_mA=%d module=0x%x rtc=0x%x persisted_capacity_present=%u\n",
            decoded,mv,ma,module,clock,a==Ok && !fuelFirstPowerOn(area) && (area&0xff)<=100 && ((area>>8)&0xf)<=9);
        if(decoded==Ok){setProperty("FGUVoltageMillivolts",(unsigned)mv,32);setProperty("FGUCurrentMilliamps",(unsigned)ma,32);}
        Result pr=adi.read(0xc28,pocv),qr=adi.read(0xc74,qmax);int ocv=0,openMa=0,bootPercent=-1;
        Result soc=Invalid;int savedDeci=-1;const unsigned savedCapacity=area&0xfff,bootMode=(area>>12)&0xf;
        if(a==Ok && fuelSavedDeciPercent(area,savedDeci)==Ok){bootPercent=savedDeci/10;soc=Ok;}
        else if(a==Ok && fuelFirstPowerOn(area) && decoded==Ok && pr==Ok && qr==Ok)
            soc=fuelBootOcv(pocv,qmax,fuelCal,134000,ocv,openMa)==Ok?
                fuelOcvCapacity(kFactoryOcvCapacity,sizeof(kFactoryOcvCapacity)/sizeof(kFactoryOcvCapacity[0]),(unsigned)ocv,bootPercent):Invalid;
        IOLog("M113 fuel: soc=%d area=0x%x mode=0x%x saved_raw=%u saved_deci=%d first=%u pocv=%u qmax=%u ocv_uV=%d open_mA=%d estimate=%d source=%s reads=%d/%d/%d\n",
            soc,area,bootMode,savedCapacity,savedDeci,fuelFirstPowerOn(area),pocv,qmax,ocv,openMa,bootPercent,
            fuelFirstPowerOn(area)?"factory-OCV-unpublished":"persisted",a,pr,qr);
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
    identifyRadioGpu();
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
    if(battery){battery->terminate(kIOServiceSynchronous);if(batteryStarted)battery->stop(this);battery->detach(this);battery->release();battery=nullptr;batteryStarted=false;}
    if(pollingAssertion && sleepRoot){sleepRoot->releasePMAssertion(pollingAssertion);pollingAssertion=kIOPMUndefinedDriverAssertionID;sleepRoot=nullptr;}
    IOService **devices[]={&touch,&keys};bool started[]={touchStarted,keysStarted};
    for(unsigned i=0;i<2;i++){IOService **d=devices[i];if(*d){if((*d)->getProvider()==this){if(started[i])(*d)->stop(this);(*d)->detach(this);}(*d)->release();*d=nullptr;}}
    touchStarted=keysStarted=false;
    if(touchPersonality){gIOCatalogue->removeDrivers(touchPersonality,false);OSSafeReleaseNULL(touchPersonality);}
    restore();for(auto &m:maps)OSSafeReleaseNULL(m);OSSafeReleaseNULL(loop);IOService::stop(p);
}
