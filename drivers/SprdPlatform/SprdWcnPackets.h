// SPDX-License-Identifier: GPL-2.0-only
// Wire framing from Unisoc's sdiohal_{common,rx}.c and sc2355/cmdevt.c,
// Hadenix/android_kernel_realme_ums512@c7f66a0; verified against the original
// 1h190 kernel and sprdwl_ng.ko. Linux SC23xx@464b3e7 confirms the CMD ABI.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "SprdBus.h"

namespace SprdWcn {
using namespace SprdBus;
constexpr size_t kPacketBlock=840,kReceiveBytes=1680,kCommandHeader=12;
inline uint16_t le16(const uint8_t *p){return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
inline uint32_t le32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
inline void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}

struct WifiReply {
    uint8_t payload[320]={};size_t length=0;
    int8_t status=0;unsigned frames=0,unmatched=0;
    uint32_t publicHeader=0,validBytes=0,remainingBytes=0;
};

inline Result encodeWifiCommand(uint8_t *out,size_t capacity,uint8_t command,uint32_t transaction,
                                const uint8_t *payload,size_t bytes,size_t &used){
    if(!out || capacity<kPacketBlock || bytes>kPacketBlock-20 || (bytes && !payload))return Invalid;
    for(size_t i=0;i<kPacketBlock;i++)out[i]=0;
    const size_t commandBytes=kCommandHeader+bytes;
    // TX channel8; original receive channels are subtype+12. Consequently the
    // command response is channel22/subtype10, not subtype6 from other BSPs.
    put32(out,(8u<<24)|(uint32_t)(commandBytes<<7));
    out[4]=0x10;out[5]=command;out[6]=(uint8_t)commandBytes;out[7]=(uint8_t)(commandBytes>>8);
    put32(out+8,transaction);
    for(size_t i=0;i<bytes;i++)out[16+i]=payload[i];
    put32(out+4+((commandBytes+3)&~size_t(3)),1u<<23);
    used=kPacketBlock;return Ok;
}

inline Result decodeWifiReply(const uint8_t *data,size_t bytes,uint8_t command,uint32_t transaction,
                              WifiReply &reply,bool &matched){
    matched=false;
    if(!data || bytes<12 || bytes%kPacketBlock)return Invalid;
    const size_t limit=bytes-8;
    reply.validBytes=le32(data+limit);reply.remainingBytes=le32(data+limit+4);
    if(reply.validBytes>limit)return Protocol;
    if(!reply.validBytes)return Ok;
    size_t at=0,payloadBytes=0;
    while(at+4<=limit && payloadBytes<reply.validBytes){
        uint32_t header=le32(data+at);reply.publicHeader=header;
        if(header&(1u<<23))break;
        unsigned type=header>>28,subtype=(header>>24)&15;
        size_t length=(header>>7)&0xffff,wireLength=length+((header&64)?2:0);
        if(!length || wireLength>1676 || wireLength>limit-at-4)return Protocol;
        if(type!=15){
            payloadBytes+=length;reply.frames++;
            const uint8_t *p=data+at+4;
            if(type==0 && subtype==10 && length>=kCommandHeader &&
               (p[0]&7)==0 && p[1]==command && le32(p+4)==transaction){
                size_t declared=le16(p+2);
                if(declared<kCommandHeader || declared>length || declared-kCommandHeader>sizeof(reply.payload))return Protocol;
                reply.status=(int8_t)p[8];reply.length=declared-kCommandHeader;
                for(size_t i=0;i<reply.length;i++)reply.payload[i]=p[kCommandHeader+i];
                matched=true;return reply.status<0?Protocol:Ok;
            }
            reply.unmatched++;
        }
        at+=4+((wireLength+3)&~size_t(3));
    }
    return Ok;
}

template<class Host,class Gpio> Result wifiCommand(Host &host,Gpio &gpio,uint8_t command,uint32_t transaction,
                            const uint8_t *payload,size_t length,WifiReply &reply){
    uint8_t tx[kPacketBlock]={},rx[kReceiveBytes]={};size_t used=0;
    Result result=encodeWifiCommand(tx,sizeof(tx),command,transaction,payload,length,used);
    if(result!=Ok)return result;
    result=host.packets(true,tx,used);if(result!=Ok)return result;
    // Factory m2-to-ap-irq-gpios is GPIO96, active high. The caller owns its
    // saved direction/input/mask and the slave's dedicated interrupt enables.
    unsigned aggregates=0;
    for(unsigned elapsed=0;elapsed<3000;elapsed++){
        if(gpio.read32(6*0x80)&1u){
            if(++aggregates>64)return Protocol;
            result=host.packets(false,rx,sizeof(rx));if(result!=Ok)return result;
            bool matched=false;result=decodeWifiReply(rx,sizeof(rx),command,transaction,reply,matched);
            if(result!=Ok || matched)return result;
        }
        gpio.delay(1000);
    }
    return Timeout;
}
}
