// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "SprdWcnPackets.h"

namespace SprdWcn {
constexpr uint8_t kWifiCmdScan=11;
constexpr uint8_t kWifiEventScanDone=0x82;
constexpr uint8_t kWifiEventMgmtFrame=0x83;
constexpr size_t kWifiBssCapacity=64;
constexpr size_t kWifiIeCapacity=384;

inline void putLe16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}

struct WifiBss {
    uint8_t bssid[6],ssid[32],ssidLength,channel;
    int8_t signalDbm;
    uint16_t capabilities,beaconInterval;
    uint32_t frequencyMhz;
    uint16_t ieLength;
    uint8_t informationElements[kWifiIeCapacity];
};

struct WifiScanReport {
    WifiBss bss[kWifiBssCapacity];
    unsigned uniqueNetworks,scanFrames,events,otherEvents,invalidFrames,aggregates,droppedNetworks;
    uint32_t responseCode;
    int8_t responseStatus;
    uint8_t doneType,responseContext;
    bool responseSeen,doneSeen;
};

inline Result encodeWifiScanRequest(uint8_t *out,size_t capacity,uint32_t channels,
                                    const uint8_t *ssid,size_t ssidLength,
                                    const uint16_t *channels5g,size_t channels5gCount,
                                    size_t &used){
    used=0;
    if(!out || capacity<12 || ssidLength>32 || (ssidLength && !ssid) ||
       channels5gCount>64 || (channels5gCount && !channels5g))return Invalid;
    const size_t bytes=10+ssidLength+2+channels5gCount*2;
    if(bytes>capacity)return Invalid;
    for(size_t i=0;i<bytes;i++)out[i]=0;
    put32(out,channels);
    putLe16(out+8,(uint16_t)ssidLength);
    for(size_t i=0;i<ssidLength;i++)out[10+i]=ssid[i];
    putLe16(out+10+ssidLength,(uint16_t)channels5gCount);
    for(size_t i=0;i<channels5gCount;i++)putLe16(out+12+ssidLength+2*i,channels5g[i]);
    used=bytes;
    return Ok;
}

inline uint32_t wifiFrequency(uint8_t channel){
    if(channel>=1 && channel<=13)return 2407+5u*channel;
    if(channel==14)return 2484;
    if(channel>=32 && channel<=196)return 5000+5u*channel;
    return 0;
}

inline Result addWifiScanFrame(WifiScanReport &report,const uint8_t *event,size_t bytes){
    if(!event || bytes<12)return Invalid;
    const uint8_t frameType=event[0],eventChannel=event[1];
    const int8_t signal=(int8_t)event[2];
    const size_t frameBytes=le16(event+10);
    if(frameBytes>bytes-12)return Protocol;
    if(frameType!=4)return Ok;
    report.scanFrames++;
    const uint8_t *frame=event+12;
    if(frameBytes<36){report.invalidFrames++;return Protocol;}
    const uint16_t fc=le16(frame);
    const unsigned subtype=(fc>>4)&15u;
    if((fc&3u)!=0 || ((fc>>2)&3u)!=0 || (subtype!=5 && subtype!=8)){
        report.invalidFrames++;return Protocol;
    }

    uint8_t bssid[6];uint8_t nonzero=0;
    for(unsigned i=0;i<6;i++){bssid[i]=frame[16+i];nonzero|=bssid[i];}
    if(!nonzero || (bssid[0]&1u)){report.invalidFrames++;return Protocol;}

    size_t at=36,ieStored=0;uint8_t ssid[32]={},ssidLength=0,dsChannel=0;
    bool haveSsid=false,ieStorageFull=false;
    while(at<frameBytes){
        if(frameBytes-at<2){report.invalidFrames++;return Protocol;}
        const uint8_t id=frame[at],length=frame[at+1];
        const size_t ieSize=(size_t)length+2;
        at+=2;
        if(length>frameBytes-at){report.invalidFrames++;return Protocol;}
        if(!ieStorageFull && ieSize<=kWifiIeCapacity-ieStored)ieStored+=ieSize;
        else ieStorageFull=true;
        if(id==0 && !haveSsid){
            if(length>sizeof(ssid)){report.invalidFrames++;return Protocol;}
            ssidLength=length;
            for(unsigned i=0;i<length;i++)ssid[i]=frame[at+i];
            haveSsid=true;
        }else if(id==3 && length==1){
            dsChannel=frame[at];
        }
        at+=length;
    }

    const uint8_t channel=eventChannel?eventChannel:dsChannel;
    const uint32_t frequency=wifiFrequency(channel);
    if(!frequency){report.invalidFrames++;return Protocol;}
    unsigned index=report.uniqueNetworks;
    for(unsigned i=0;i<report.uniqueNetworks;i++){
        bool same=report.bss[i].frequencyMhz==frequency;
        for(unsigned j=0;j<6 && same;j++)same=report.bss[i].bssid[j]==bssid[j];
        if(same){index=i;break;}
    }
    if(index==report.uniqueNetworks){
        if(report.uniqueNetworks>=kWifiBssCapacity){report.droppedNetworks++;return Ok;}
        report.uniqueNetworks++;
    }

    WifiBss &b=report.bss[index];
    for(unsigned i=0;i<6;i++)b.bssid[i]=bssid[i];
    for(unsigned i=0;i<32;i++)b.ssid[i]=ssid[i];
    b.ssidLength=ssidLength;b.channel=channel;b.frequencyMhz=frequency;b.signalDbm=signal;
    b.beaconInterval=le16(frame+32);b.capabilities=le16(frame+34);
    b.ieLength=(uint16_t)ieStored;
    size_t copied=0;at=36;
    while(at<frameBytes && copied<ieStored){
        const size_t ieSize=(size_t)frame[at+1]+2;
        if(ieSize>ieStored-copied)break;
        for(size_t i=0;i<ieSize;i++)b.informationElements[copied+i]=frame[at+i];
        copied+=ieSize;at+=ieSize;
    }
    return Ok;
}

inline Result decodeWifiScanAggregate(const uint8_t *data,size_t bytes,uint32_t transaction,
                                     uint8_t context,WifiScanReport &report){
    if(!data || bytes!=kReceiveBytes)return Invalid;
    report.aggregates++;
    const size_t validBytes=le32(data+bytes-8);
    if(validBytes>kReceivePacketBytes)return Protocol;
    size_t at=0,consumed=0;
    while(at+4<=kReceivePacketBytes && consumed<validBytes){
        const uint32_t header=le32(data+at);
        if(header&(1u<<23))break;
        const unsigned type=header>>28,subtype=(header>>24)&15u;
        const size_t length=(header>>7)&0xffffu;
        const size_t wireLength=length+((header&64u)?2u:0u);
        if(!length || wireLength>kReceivePacketBytes-at-4 || length>validBytes-consumed)return Protocol;
        if(type==0 && subtype==10){
            const uint8_t *p=data+at+4;
            if(length<kCommandHeader)return Protocol;
            const uint8_t messageType=p[0]&7u;
            const size_t declared=le16(p+2);
            if(declared<kCommandHeader || declared>length)return Protocol;
            if(messageType==0 && p[1]==kWifiCmdScan && le32(p+4)==transaction){
                if(!(p[0]&0x10u) || ((p[0]>>5)&7u)!=context)return Protocol;
                report.responseSeen=true;report.responseStatus=(int8_t)p[8];
                report.responseContext=(p[0]>>5)&7u;
                if(declared>=kCommandHeader+4)report.responseCode=le32(p+kCommandHeader);
                if(report.responseStatus)return Protocol;
            }else if(messageType==1){
                report.events++;
                if(((p[0]>>5)&7u)!=context)report.otherEvents++;
                else{
                    const uint8_t *event=p+kCommandHeader;
                    const size_t eventBytes=declared-kCommandHeader;
                    if(p[1]==kWifiEventScanDone){
                        if(eventBytes<1)return Protocol;
                        report.doneSeen=true;report.doneType=event[0];
                        if(report.doneType!=1)return Protocol;
                    }else if(p[1]==kWifiEventMgmtFrame){
                        Result r=addWifiScanFrame(report,event,eventBytes);
                        if(r!=Ok)return r;
                    }else report.otherEvents++;
                }
            }
        }
        consumed+=length;
        at+=4+((wireLength+3u)&~size_t(3));
    }
    return Ok;
}

template<class Host,class Gpio> Result wifiScan(Host &host,Gpio &gpio,uint8_t context,
                                                uint32_t transaction,uint32_t channels,
                                                WifiScanReport &report){
    uint8_t body[16]={},tx[kTransmitBytes]={},rx[kReceiveBytes]={};
    size_t bodyBytes=0,txBytes=0;
    Result r=encodeWifiScanRequest(body,sizeof(body),channels,nullptr,0,nullptr,0,bodyBytes);
    if(r!=Ok)return r;
    r=encodeWifiCommand(tx,sizeof(tx),kWifiCmdScan,transaction,body,bodyBytes,txBytes,context);
    if(r!=Ok)return r;
    r=host.packets(true,tx,txBytes);
    if(r!=Ok)return r;
    unsigned aggregates=0;
    for(unsigned elapsed=0;elapsed<25000;elapsed++){
        if(gpio.read32(6*0x80)&1u){
            if(++aggregates>128)return Protocol;
            r=host.packets(false,rx,sizeof(rx));
            if(r!=Ok)return r;
            r=decodeWifiScanAggregate(rx,sizeof(rx),transaction,context,report);
            if(r!=Ok)return r;
            if(report.responseSeen && report.doneSeen)return Ok;
        }
        gpio.delay(1000);
    }
    return Timeout;
}
}
