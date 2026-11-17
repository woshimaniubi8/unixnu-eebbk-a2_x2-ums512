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
#include <kern/clock.h>
#include <mach/mach_time.h>

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
    unsigned scans=0,drained=0,ticks=0;
    bool pendingJoin=false,joining=false,pendingDisconnect=false,secured=false;
    SprdWcn::StationJoin join={};
    uint64_t joinDeadline=0;
    struct Key {uint8_t body[43];bool pairwise;};
    Key keys[4]={};unsigned keyHead=0,keyCount=0;
    struct Frame {uint16_t bytes;uint8_t data[SprdWcn::kEthernetMaximum];};
    struct Queue {Frame frame[32];} *tx=nullptr;
    unsigned txHead=0,txCount=0,txSent=0,txFailed=0,txFull=0,rxFrames=0;
} state;
bool metaReady=false;
uint64_t nowNs(){uint64_t now=0;absolutetime_to_nanoseconds(mach_absolute_time(),&now);return now;}
void eraseSecret(void *p,size_t n){volatile uint8_t *v=(volatile uint8_t *)p;while(n--)*v++=0;}
void connected(void *,bool success,unsigned reason,const uint8_t *bssid,uint8_t channel,int8_t signal) {
    void (*done)(int)=nullptr;
    IOLockLock(state.lock);
    if(state.ready && state.joining) {
        state.joining=false;state.status.result=success?NW_RES_OK:NW_RES_REJECTED;
        state.status.state=success?(state.secured?NW_STA_KEYS:NW_STA_UP):NW_STA_IDLE;
        if(success){memcpy(state.status.bssid,bssid,6);state.status.freq_mhz=SprdWcn::wifiFrequency(channel);state.status.signal_dbm=signal;}
        done=state.events.connect_done;
    }
    IOLockUnlock(state.lock);
    IOLog("M136 wifi: association event success=%u reason=%u channel=%u\n",success,reason,channel);
    if(done)done(success?NW_RES_OK:NW_RES_REJECTED);
}
void disconnected(void *,unsigned reason) {
    void (*lost)(int)=nullptr;
    IOLockLock(state.lock);
    if(state.ready){state.joining=false;state.status.state=NW_STA_IDLE;state.txCount=0;
        eraseSecret(state.keys,sizeof(state.keys));state.keyCount=0;lost=state.events.link_lost;}
    IOLockUnlock(state.lock);
    IOLog("M136 wifi: disconnect event reason=%u\n",reason);
    if(lost)lost((int)reason);
}
void received(void *,const uint8_t *frame,size_t length) {
    void (*rx)(const uint8_t *,size_t)=nullptr;
    IOLockLock(state.lock);
    if(state.ready && state.status.state>=NW_STA_KEYS) {
        const bool eapol=length>=14 && frame[12]==0x88 && frame[13]==0x8e;
        if(state.status.state==NW_STA_UP || eapol){state.rxFrames++;rx=state.events.rx;}
    }
    IOLockUnlock(state.lock);
    if(rx)rx(frame,length);
}
bool hasMetaClass(const char *name) {
    const OSSymbol *symbol=OSSymbol::withCString(name);if(!symbol)return true;
    bool present=OSMetaClass::getMetaClassWithName(symbol)!=nullptr;
    symbol->release();return present;
}

void tick(OSObject *,IOTimerEventSource *) {
    if(!state.ready)return;
    bool scan=false,power=false,on=false,join=false,leave=false,key=false;
    uint8_t ssid[32]={};size_t length=0;
    SprdWcn::StationJoin request;State::Key keyRequest={};
    IOLockLock(state.lock);
    if(state.ready) {
        power=state.pendingPower;on=state.powerRequested;state.pendingPower=false;
        if(!power && state.pendingDisconnect){leave=true;state.pendingDisconnect=false;}
        if(!power && !leave && state.pendingJoin){join=true;request=state.join;
            eraseSecret(&state.join,sizeof(state.join));state.pendingJoin=false;state.joining=true;
            state.status.state=NW_STA_AUTH;state.joinDeadline=nowNs()+20000000000ull;}
        if(!power && !leave && !join && state.keyCount) {
            key=true;keyRequest=state.keys[state.keyHead];eraseSecret(&state.keys[state.keyHead],sizeof(keyRequest));
            state.keyHead=(state.keyHead+1)%4;state.keyCount--;
        }
        if(!power && !leave && !join && !key && state.pendingScan && state.status.state!=NW_STA_OFF) {
            scan=true;state.pendingScan=false;state.scanning=true;
            state.status.state=NW_STA_SCANNING;
            length=state.scanSsidLength;memcpy(ssid,state.scanSsid,length);
        }
    }
    IOLockUnlock(state.lock);
    if(power) {
        SprdBus::Result result=state.hardware->power(on);
        IOLockLock(state.lock);
        if(result==SprdBus::Ok){state.status.state=on?NW_STA_IDLE:NW_STA_OFF;state.joining=false;
            state.txCount=0;eraseSecret(state.keys,sizeof(state.keys));state.keyCount=0;}
        state.status.result=result==SprdBus::Ok?NW_RES_OK:NW_RES_FAILED;
        IOLockUnlock(state.lock);
        IOLog("M130 wifi: power requested=%u result=%d\n",on,result);
        if(result==SprdBus::Ok)sprd_wifi_net_power_done(on);
    } else if(leave) {
        SprdBus::Result result=state.hardware->disconnect(3);
        IOLog("M136 wifi: disconnect command result=%d\n",result);
        if(result!=SprdBus::Ok)disconnected(nullptr,3);
    } else if(join) {
        SprdBus::Result result=state.hardware->connect(request);
        eraseSecret(&request,sizeof(request));
        IOLog("M136 wifi: CONNECT acknowledgement=%d; link waits for firmware event\n",result);
        if(result!=SprdBus::Ok)connected(nullptr,false,0,nullptr,0,0);
    } else if(key) {
        SprdBus::Result result=state.hardware->key(keyRequest.body,sizeof(keyRequest.body));
        const bool pairwise=keyRequest.pairwise;eraseSecret(&keyRequest,sizeof(keyRequest));
        IOLockLock(state.lock);
        if(result==SprdBus::Ok && pairwise && state.status.state==NW_STA_KEYS)state.status.state=NW_STA_UP;
        IOLockUnlock(state.lock);
        IOLog("M136 wifi: install CCMP key pairwise=%u result=%d\n",pairwise,result);
        if(result!=SprdBus::Ok){state.hardware->disconnect(3);disconnected(nullptr,3);}
    } else if(scan) {
        bzero(state.work,sizeof(*state.work));
        SprdBus::Result result=state.hardware->scan(ssid,length,*state.work);
        void (*done)()=nullptr;
        IOLockLock(state.lock);
        if(result==SprdBus::Ok)memcpy(state.cache,state.work,sizeof(*state.cache));
        state.scanning=false;if(state.status.state==NW_STA_SCANNING)state.status.state=NW_STA_IDLE;
        state.status.result=result==SprdBus::Ok?NW_RES_OK:NW_RES_FAILED;
        state.scans++;done=state.events.scan_done;
        IOLockUnlock(state.lock);
        IOLog("M130 wifi: system scan=%u result=%d done=%u frames=%u bss=%u invalid=%u\n",
              state.scans,result,state.work->doneSeen,state.work->scanFrames,
              state.work->uniqueNetworks,state.work->invalidFrames);
        if(done)done();
    }
    // Every control transaction dispatches asynchronous frames; the idle drain
    // uses the same owner and preserves DTBS continuation across timer turns.
    if(!scan) {
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
    bool timedOut=false;
    IOLockLock(state.lock);timedOut=state.joining && nowNs()>=state.joinDeadline;IOLockUnlock(state.lock);
    if(timedOut){state.hardware->disconnect(3);connected(nullptr,false,NW_RES_TIMEOUT,nullptr,0,0);}
    for(unsigned budget=0;budget<8;budget++) {
        State::Frame frame={};bool present=false;
        IOLockLock(state.lock);
        if(state.tx && state.txCount){frame=state.tx->frame[state.txHead];present=true;}
        IOLockUnlock(state.lock);if(!present)break;
        SprdBus::Result result=state.hardware->transmit(frame.data,frame.bytes);
        if(result==SprdBus::Busy)break;
        IOLockLock(state.lock);
        if(state.txCount){state.txHead=(state.txHead+1)%32;state.txCount--;}
        if(result==SprdBus::Ok)state.txSent++;else state.txFailed++;
        IOLockUnlock(state.lock);
        if(result!=SprdBus::Ok)IOLog("M136 wifi: Ethernet transmit failed result=%d\n",result);
    }
    if((++state.ticks%250)==0) {
        IOLockLock(state.lock);
        unsigned current=state.status.state,scans=state.scans,packets=state.drained;
        IOLockUnlock(state.lock);
        IOLog("M133 wifi: worker ticks=%u packets=%u scans=%u state=%u\n",
              state.ticks,packets,scans,current);
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
    else if(state.pendingScan || state.scanning || state.joining || state.pendingJoin || state.status.state>=NW_STA_AUTH)result=EBUSY;
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
    else if(state.pendingPower || state.pendingScan || state.scanning || state.pendingJoin)result=EBUSY;
    else {state.pendingPower=true;state.powerRequested=on!=0;}
    IOLockUnlock(state.lock);return result;
}
extern "C" int nw_wifi_bss_clear() {
    if(!state.lock)return ENETDOWN;
    IOLockLock(state.lock);if(state.cache)bzero(state.cache,sizeof(*state.cache));
    IOLockUnlock(state.lock);return 0;
}
extern "C" int nw_wifi_connect(const uint8_t *ssid,size_t length,const uint8_t *bssid,const char *pmk,size_t pmkLength) {
    if(!ssid || !length || length>32 || (pmkLength && (!pmk || pmkLength!=32)))return EINVAL;
    if(!state.lock)return ENETDOWN;
    IOLockLock(state.lock);int result=0;bool found=false;
    if(!state.ready || state.status.state==NW_STA_OFF)result=ENETDOWN;
    else if(state.pendingJoin || state.joining || state.scanning || state.pendingScan || state.pendingPower)result=EBUSY;
    else if(state.cache) {
        unsigned best=0;int signal=-200;
        for(unsigned i=0;i<state.cache->uniqueNetworks;i++) {
            const auto &b=state.cache->bss[i];
            if(b.ssidLength!=length || memcmp(b.ssid,ssid,length) || (bssid && memcmp(b.bssid,bssid,6)))continue;
            if(b.signalDbm>signal){best=i;signal=b.signalDbm;found=true;}
        }
        if(found) {
            SprdWcn::StationJoin candidate={};candidate.bss=state.cache->bss[best];
            candidate.hasPmk=pmkLength!=0;if(pmkLength)memcpy(candidate.pmk,pmk,32);
            uint8_t body[82]={},ie[22]={};size_t ieBytes=0;
            if(SprdWcn::encodeStationConnect(candidate,body,ie,ieBytes)!=SprdBus::Ok)result=EOPNOTSUPP;
            else {state.join=candidate;state.pendingJoin=true;state.secured=ieBytes!=0;state.txCount=0;
                state.status.state=NW_STA_AUTH;
                state.status.ssid_len=(uint8_t)length;memcpy(state.status.ssid,ssid,length);}
            eraseSecret(&candidate,sizeof(candidate));eraseSecret(body,sizeof(body));
        }else result=ENOENT;
    }else result=ENOENT;
    IOLockUnlock(state.lock);return result;
}
extern "C" int nw_wifi_disconnect() {
    if(!state.lock)return ENETDOWN;
    IOLockLock(state.lock);int result=0;
    if(!state.ready)result=ENETDOWN;
    else {state.pendingDisconnect=state.status.state>=NW_STA_AUTH;state.pendingJoin=false;
        eraseSecret(&state.join,sizeof(state.join));}
    IOLockUnlock(state.lock);return result;
}
extern "C" int nw_wifi_key(uint8_t index,int pairwise,const uint8_t *address,
                            const uint8_t *key,size_t length,const uint8_t *sequence,size_t sequenceLength) {
    if(index>3 || !key || length!=16 || sequenceLength>8 || (sequenceLength && !sequence))return EINVAL;
    if(!state.lock)return ENETDOWN;
    IOLockLock(state.lock);int result=0;
    if(!state.ready || state.status.state<NW_STA_KEYS)result=ENETDOWN;
    else if(state.keyCount==4)result=ENOBUFS;
    else {
        State::Key &k=state.keys[(state.keyHead+state.keyCount)%4];k={};k.pairwise=pairwise!=0;
        k.body[0]=3;k.body[1]=index;k.body[2]=pairwise!=0;
        if(address)memcpy(k.body+3,address,6);else if(pairwise)memcpy(k.body+3,state.status.bssid,6);
        if(sequenceLength)memcpy(k.body+9,sequence,sequenceLength);
        k.body[25]=4;k.body[26]=16;memcpy(k.body+27,key,16);state.keyCount++;
    }
    IOLockUnlock(state.lock);return result;
}
extern "C" int nw_wifi_tx(const uint8_t *frame,size_t length) {
    if(!frame || length<14 || length>SprdWcn::kEthernetMaximum)return EMSGSIZE;
    if(!state.lock)return ENETDOWN;
    IOLockLock(state.lock);int result=0;
    const bool eapol=frame[12]==0x88 && frame[13]==0x8e;
    if(!state.ready || !state.tx || (state.status.state!=NW_STA_UP && !(state.status.state==NW_STA_KEYS && eapol)))result=ENETDOWN;
    else if(state.txCount==32){state.txFull++;result=ENOBUFS;}
    else {auto &slot=state.tx->frame[(state.txHead+state.txCount)%32];
        slot.bytes=(uint16_t)length;memcpy(slot.data,frame,length);state.txCount++;}
    IOLockUnlock(state.lock);return result;
}
extern "C" void nw_wifi_tx_reasons(char *text,int size) {
    if(!text || size<=0 || !state.lock)return;
    IOLockLock(state.lock);snprintf(text,(size_t)size,"sent=%u failed=%u full=%u queued=%u rx=%u",
        state.txSent,state.txFailed,state.txFull,state.txCount,state.rxFrames);IOLockUnlock(state.lock);
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
    state.tx=(State::Queue *)IOMalloc(sizeof(*state.tx));
    if(!state.lock || !state.loop || !state.cache || !state.work || !state.tx){sprdWifiRuntimeStop();return false;}
    bzero(state.cache,sizeof(*state.cache));bzero(state.work,sizeof(*state.work));
    if(initial)memcpy(state.cache,initial,sizeof(*state.cache));
    state.hardware=hardware;state.status={};state.status.state=NW_STA_IDLE;
    state.scans=state.drained=state.ticks=0;
    state.status.channels=0x421;memcpy(state.status.mac,mac,6);state.ready=true;
    hardware->callbacks({nullptr,connected,disconnected,received,nullptr});
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
    if(state.hardware)state.hardware->callbacks({});
    OSSafeReleaseNULL(state.timer);
    if(state.gate && state.loop)state.loop->removeEventSource(state.gate);
    OSSafeReleaseNULL(state.gate);
    OSSafeReleaseNULL(state.loop);
    auto *cache=state.cache,*work=state.work;auto *tx=state.tx;
    if(state.lock)IOLockLock(state.lock);
    state.cache=nullptr;state.work=nullptr;state.hardware=nullptr;state.tx=nullptr;
    state.pendingScan=false;state.scanning=false;state.pendingPower=false;
    state.pendingJoin=false;state.joining=false;state.pendingDisconnect=false;
    eraseSecret(&state.join,sizeof(state.join));eraseSecret(state.keys,sizeof(state.keys));
    state.keyCount=state.keyHead=state.txCount=state.txHead=0;
    state.status={};state.status.state=NW_STA_OFF;state.events={};
    if(state.lock)IOLockUnlock(state.lock);
    if(cache)IOFree(cache,sizeof(*cache));
    if(work)IOFree(work,sizeof(*work));
    if(tx){eraseSecret(tx,sizeof(*tx));IOFree(tx,sizeof(*tx));}
    // The C networking callbacks are built in and can outlive an asynchronous
    // ifnet detach. Their single global lock has the same kernel lifetime;
    // it remains valid while they observe the stopped, empty state.
}
