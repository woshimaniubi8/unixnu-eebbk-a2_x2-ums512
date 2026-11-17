// SPDX-License-Identifier: GPL-2.0-only
#include "SprdWifiRuntime.h"
#include "SprdWifiNetABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include <sys/errno.h>
#include <stdarg.h>

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void sprd_wifi_net_static_init() __asm__("__GLOBAL__sub_I_SprdWifiNet.cpp");
extern "C" bool sprd_wifi_net_start(const uint8_t *);
extern "C" void sprd_wifi_net_stop();
extern "C" void sprd_wifi_net_power_done(bool);
namespace {
struct State {
    IOLock *lock=nullptr;
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr;
    IOCommandGate *gate=nullptr;
    SprdWifiHardware *hardware=nullptr;
    SprdWcn::WifiScanReport *cache=nullptr,*work=nullptr;
    nw_wifi_events events={};
    nw_wifi_status status={};
    bool ready=false,pendingScan=false,scanning=false,pendingPower=false,powerRequested=true;
    uint8_t scanSsid[32]={};
    size_t scanSsidLength=0;
    unsigned scans=0,drained=0;
} state;
bool metaReady=false;
bool hasMetaClass(const char *name) {
    const OSSymbol *symbol=OSSymbol::withCString(name);if(!symbol)return true;
    bool present=OSMetaClass::getMetaClassWithName(symbol)!=nullptr;
    symbol->release();return present;
}

void tick(OSObject *,IOTimerEventSource *) {
    if(!state.ready)return;
    bool scan=false,power=false,on=false;
    uint8_t ssid[32]={};size_t length=0;
    IOLockLock(state.lock);
    if(state.ready) {
        power=state.pendingPower;on=state.powerRequested;state.pendingPower=false;
        if(!power && state.pendingScan && state.status.state!=NW_STA_OFF) {
            scan=true;state.pendingScan=false;state.scanning=true;
            state.status.state=NW_STA_SCANNING;
            length=state.scanSsidLength;memcpy(ssid,state.scanSsid,length);
        }
    }
    IOLockUnlock(state.lock);
    if(power) {
        SprdBus::Result result=state.hardware->power(on);
        IOLockLock(state.lock);
        if(result==SprdBus::Ok)state.status.state=on?NW_STA_IDLE:NW_STA_OFF;
        state.status.result=result==SprdBus::Ok?NW_RES_OK:NW_RES_FAILED;
        IOLockUnlock(state.lock);
        IOLog("M130 wifi: power requested=%u result=%d\n",on,result);
        if(result==SprdBus::Ok)sprd_wifi_net_power_done(on);
    } else if(scan) {
        bzero(state.work,sizeof(*state.work));
        SprdBus::Result result=state.hardware->scan(ssid,length,*state.work);
        void (*done)()=nullptr;
        IOLockLock(state.lock);
        if(result==SprdBus::Ok)memcpy(state.cache,state.work,sizeof(*state.cache));
        state.scanning=false;state.status.state=NW_STA_IDLE;
        state.status.result=result==SprdBus::Ok?NW_RES_OK:NW_RES_FAILED;
        state.scans++;done=state.events.scan_done;
        IOLockUnlock(state.lock);
        IOLog("M130 wifi: system scan=%u result=%d done=%u frames=%u bss=%u invalid=%u\n",
              state.scans,result,state.work->doneSeen,state.work->scanFrames,
              state.work->uniqueNetworks,state.work->invalidFrames);
        if(done)done();
    } else {
        unsigned packets=0;
        SprdBus::Result result=state.hardware->drain(packets);
        state.drained+=packets;
        if(result!=SprdBus::Ok) {
            IOLockLock(state.lock);state.ready=false;state.status.state=NW_STA_OFF;
            state.status.result=NW_RES_FAILED;IOLockUnlock(state.lock);
            IOLog("M130 wifi: receive stopped result=%d packets=%u; no further commands issued\n",result,state.drained);
            sprd_wifi_net_power_done(false);
            return;
        }
    }
    if(state.timer)state.timer->setTimeoutMS(20);
}
IOReturn stopTick(OSObject *,void *,void *,void *,void *) {
    IOLockLock(state.lock);state.ready=false;IOLockUnlock(state.lock);
    if(state.timer){state.timer->cancelTimeout();state.timer->disable();}
    return kIOReturnSuccess;
}
}

extern "C" void native_wifi_logf(int,const char *fmt,...) {
    char line[256];va_list ap;va_start(ap,fmt);
    vsnprintf(line,sizeof(line),fmt,ap);va_end(ap);IOLog("%s\n",line);
}
extern "C" void nw_wifi_set_events(const nw_wifi_events *events) {
    if(!state.lock)return;
    IOLockLock(state.lock);
    if(events)state.events=*events;else bzero(&state.events,sizeof(state.events));
    IOLockUnlock(state.lock);
}
extern "C" int nw_wifi_scan(const uint8_t *ssid,size_t length) {
    if(length>32 || (length && !ssid) || !state.lock)return EINVAL;
    IOLockLock(state.lock);int result=0;
    if(!state.ready || state.status.state==NW_STA_OFF)result=ENETDOWN;
    else if(state.pendingScan || state.scanning)result=EBUSY;
    else {state.pendingScan=true;state.scanSsidLength=length;if(length)memcpy(state.scanSsid,ssid,length);}
    IOLockUnlock(state.lock);return result;
}
extern "C" int nw_wifi_bss_count() {
    if(!state.lock)return 0;
    IOLockLock(state.lock);int count=state.cache?(int)state.cache->uniqueNetworks:0;
    IOLockUnlock(state.lock);return count;
}
extern "C" int nw_wifi_bss_get(int index,nw_bss_info *out) {
    if(!out || index<0 || !state.lock)return EINVAL;
    IOLockLock(state.lock);
    if(!state.cache || (unsigned)index>=state.cache->uniqueNetworks) {
        IOLockUnlock(state.lock);return ENOENT;
    }
    const SprdWcn::WifiBss &b=state.cache->bss[index];bzero(out,sizeof(*out));
    memcpy(out->bssid,b.bssid,6);out->ssid_len=b.ssidLength;memcpy(out->ssid,b.ssid,32);
    out->freq_mhz=b.frequencyMhz;out->signal_dbm=b.signalDbm;
    out->caps=b.capabilities;out->beacon_int=b.beaconInterval;
    out->ie_len=b.ieLength;memcpy(out->ie,b.informationElements,b.ieLength);
    IOLockUnlock(state.lock);return 0;
}
extern "C" void nw_wifi_status_get(nw_wifi_status *out) {
    if(!out)return;bzero(out,sizeof(*out));if(!state.lock)return;
    IOLockLock(state.lock);*out=state.status;IOLockUnlock(state.lock);
}
extern "C" int nw_wifi_power(int on) {
    if(!state.lock)return ENETDOWN;
    IOLockLock(state.lock);int result=0;
    if(!state.ready)result=ENETDOWN;
    else if(state.pendingPower || state.scanning)result=EBUSY;
    else {state.pendingPower=true;state.powerRequested=on!=0;}
    IOLockUnlock(state.lock);return result;
}
extern "C" int nw_wifi_bss_clear() {
    if(!state.lock)return ENETDOWN;
    IOLockLock(state.lock);if(state.cache)bzero(state.cache,sizeof(*state.cache));
    IOLockUnlock(state.lock);return 0;
}
extern "C" int nw_wifi_connect(const uint8_t *,size_t,const uint8_t *,const char *,size_t) {return NW_RES_UNSUPPORTED;}
extern "C" int nw_wifi_disconnect() {
    if(!state.lock)return ENETDOWN;
    IOLockLock(state.lock);bool associated=state.status.state>=NW_STA_AUTH;
    IOLockUnlock(state.lock);return associated?EOPNOTSUPP:0;
}
extern "C" int nw_wifi_tx(const uint8_t *,size_t) {return ENETDOWN;}
extern "C" void nw_wifi_tx_reasons(char *text,int size) {
    if(text && size>0)strlcpy(text,"SC2355 Ethernet TX not yet qualified",(size_t)size);
}

bool sprdWifiRuntimeStart(IOService *owner,SprdWifiHardware *hardware,const uint8_t *mac,
                         const SprdWcn::WifiScanReport *initial) {
    if(!owner || !hardware || !mac || state.ready || state.hardware || state.loop)return false;
    if(!metaReady) {
        if(hasMetaClass("IO80211Controller") || hasMetaClass("IONetworkStack")) {
            IOLog("M130 wifi: original network metaclass already exists; adapter not registered\n");return false;
        }
        void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!handle)return false;
        sprd_wifi_net_static_init();
        OSReturn result=OSMetaClass::postModLoad(handle);
        if(result){IOLog("M130 wifi: network metaclass registration=0x%x\n",result);return false;}
        metaReady=true;
    }
    if(!state.lock)state.lock=IOLockAlloc();
    state.loop=IOWorkLoop::workLoop();
    state.cache=(SprdWcn::WifiScanReport *)IOMalloc(sizeof(*state.cache));
    state.work=(SprdWcn::WifiScanReport *)IOMalloc(sizeof(*state.work));
    if(!state.lock || !state.loop || !state.cache || !state.work){sprdWifiRuntimeStop();return false;}
    bzero(state.cache,sizeof(*state.cache));bzero(state.work,sizeof(*state.work));
    if(initial)memcpy(state.cache,initial,sizeof(*state.cache));
    state.hardware=hardware;state.status={};state.status.state=NW_STA_IDLE;
    state.status.channels=0x421;memcpy(state.status.mac,mac,6);state.ready=true;
    state.gate=IOCommandGate::commandGate(owner);
    state.timer=IOTimerEventSource::timerEventSource(owner,tick);
    if(!state.gate || !state.timer || state.loop->addEventSource(state.gate)!=kIOReturnSuccess ||
       state.loop->addEventSource(state.timer)!=kIOReturnSuccess ||
       !sprd_wifi_net_start(mac)){sprdWifiRuntimeStop();return false;}
    state.timer->setTimeoutMS(20);
    IOLog("M130 wifi: runtime ready; one SDIO owner, initial_bss=%u\n",state.cache->uniqueNetworks);
    return true;
}
void sprdWifiRuntimeStop() {
    if(state.gate && state.gate->getWorkLoop())state.gate->runAction(stopTick);
    else state.ready=false;
    if(state.timer){state.timer->cancelTimeout();if(state.loop)state.loop->removeEventSource(state.timer);}
    sprd_wifi_net_stop();
    OSSafeReleaseNULL(state.timer);
    if(state.gate && state.loop)state.loop->removeEventSource(state.gate);
    OSSafeReleaseNULL(state.gate);
    OSSafeReleaseNULL(state.loop);
    auto *cache=state.cache,*work=state.work;
    if(state.lock)IOLockLock(state.lock);
    state.cache=nullptr;state.work=nullptr;state.hardware=nullptr;
    state.pendingScan=false;state.scanning=false;state.pendingPower=false;
    state.status={};state.status.state=NW_STA_OFF;state.events={};
    if(state.lock)IOLockUnlock(state.lock);
    if(cache)IOFree(cache,sizeof(*cache));
    if(work)IOFree(work,sizeof(*work));
    // The C networking callbacks are built in and can outlive an asynchronous
    // ifnet detach. Their single global lock has the same kernel lifetime;
    // it remains valid while they observe the stopped, empty state.
}
