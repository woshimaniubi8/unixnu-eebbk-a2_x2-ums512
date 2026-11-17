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
constexpr size_t kPacketBlock=840,kTransmitBytes=1680,kReceivePacketBytes=1680,kReceiveBytes=2520,kCommandHeader=12;
// Exact 1h190 DTBO: m2-wakeup-ap-gpios=32 (packet RX), while
// m2-to-ap-irq-gpios=96 is the independent public/sleep/memory interrupt.
constexpr unsigned kPacketGpioBank=2*0x80;
inline uint16_t le16(const uint8_t *p){return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
inline uint32_t le32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
inline void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}

struct WifiReply {
    uint8_t payload[320]={};size_t length=0;
    int8_t status=0;uint8_t context=0;unsigned frames=0,unmatched=0;
    uint32_t publicHeader=0,validBytes=0,remainingBytes=0;
    uint32_t firstWords[4]={},trailer[4]={};
};

inline Result encodeWifiCommand(uint8_t *out,size_t capacity,uint8_t command,uint32_t transaction,
                                const uint8_t *payload,size_t bytes,size_t &used,uint8_t context=0){
    used=0;
    if(!out || bytes>kTransmitBytes-20 || (bytes && !payload) || context>7)return Invalid;
    const size_t commandBytes=kCommandHeader+bytes;
    const size_t eof=4+((commandBytes+3)&~size_t(3));
    const size_t padded=((eof+4+kPacketBlock-1)/kPacketBlock)*kPacketBlock;
    if(capacity<padded)return Invalid;
    for(size_t i=0;i<padded;i++)out[i]=0;
    // TX channel8; original receive channels are subtype+12. Consequently the
    // command response is channel22/subtype10, not subtype6 from other BSPs.
    put32(out,(8u<<24)|(uint32_t)(commandBytes<<7));
    out[4]=(uint8_t)(0x10|(context<<5));out[5]=command;out[6]=(uint8_t)commandBytes;out[7]=(uint8_t)(commandBytes>>8);
    put32(out+8,transaction);
    for(size_t i=0;i<bytes;i++)out[16+i]=payload[i];
    put32(out+eof,1u<<23);
    used=padded;return Ok;
}

inline Result decodeWifiReply(const uint8_t *data,size_t bytes,uint8_t command,uint32_t transaction,
                              WifiReply &reply,bool &matched){
    matched=false;
    // Original ADMA receive: one1680-byte packet slot + one840-byte DTBS
    // metadata block. Normal-mode1680 reads put metadata at the wrong offset
    // and also consume the CP FIFO using the wrong transaction length.
    if(!data || bytes!=kReceiveBytes)return Invalid;
    const size_t limit=kReceivePacketBytes;
    for(unsigned i=0;i<4;i++){reply.firstWords[i]=le32(data+i*4);reply.trailer[i]=le32(data+bytes-16+i*4);}
    reply.validBytes=le32(data+bytes-8);reply.remainingBytes=le32(data+bytes-4);
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
                reply.status=(int8_t)p[8];reply.context=p[0]>>5;reply.length=declared-kCommandHeader;
                for(size_t i=0;i<reply.length;i++)reply.payload[i]=p[kCommandHeader+i];
                matched=true;return reply.status!=0?Protocol:Ok;
            }
            reply.unmatched++;
        }
        at+=4+((wireLength+3)&~size_t(3));
    }
    return Ok;
}

template<class Host,class Gpio> Result wifiCommand(Host &host,Gpio &gpio,uint8_t command,uint32_t transaction,
                            const uint8_t *payload,size_t length,WifiReply &reply,uint8_t context=0){
    uint8_t tx[kTransmitBytes]={},rx[kReceiveBytes]={};size_t used=0;
    Result result=encodeWifiCommand(tx,sizeof(tx),command,transaction,payload,length,used,context);
    if(result!=Ok)return result;
    result=host.packets(true,tx,used);if(result!=Ok)return result;
    // Factory packet-ready GPIO32 is active high in sdiohal's IRQ handler.
    // The caller owns its
    // saved direction/input/mask and the slave's dedicated interrupt enables.
    for(unsigned elapsed=0;elapsed<3000;elapsed++){
        if(gpio.read32(kPacketGpioBank)&1u){
            result=host.packets(false,rx,sizeof(rx));if(result!=Ok)return result;
            bool matched=false;result=decodeWifiReply(rx,sizeof(rx),command,transaction,reply,matched);
            if(result!=Ok || matched)return result;
        }
        gpio.delay(1000);
    }
    return Timeout;
}
}
