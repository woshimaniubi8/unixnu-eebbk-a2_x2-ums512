// SPDX-License-Identifier: GPL-2.0-only
// H4 and VSC init order: Otto Pflueger's btsprdsipc.c, Linux464b3e7.
// SDIO layout and command padding: exact1h190 sprdbt_tty.ko, not SIPC's8.
#pragma once
#include "SprdWcnPackets.h"

namespace SprdWcn {
struct HciReply {
    uint8_t payload[252]={},status=0xff;size_t length=0;
    unsigned events=0,unmatched=0,aggregates=0;bool complete=false;
    uint32_t publicHeader=0,validBytes=0,remainingBytes=0;
};

inline Result encodeHciCommand(uint8_t *out,size_t capacity,uint16_t opcode,
                              const uint8_t *data,size_t bytes,size_t &used){
    used=0;
    if(!out || capacity<kPacketBlock || bytes>255 || (bytes&&!data))return Invalid;
    const size_t h4=bytes+4,padded=h4+4-h4%4;
    for(size_t i=0;i<kPacketBlock;i++)out[i]=0;
    put32(out,(3u<<24)|(uint32_t)(padded<<7));
    out[4]=1;out[5]=(uint8_t)opcode;out[6]=(uint8_t)(opcode>>8);out[7]=(uint8_t)bytes;
    for(size_t i=0;i<bytes;i++)out[8+i]=data[i];
    put32(out+4+padded,1u<<23);used=kPacketBlock;return Ok;
}

class HciEvents {
    uint8_t event[258]={};size_t used=0,needed=3;
public:
    Result consume(const uint8_t *data,size_t bytes,uint16_t opcode,HciReply &reply){
        if(!data && bytes)return Invalid;
        for(size_t i=0;i<bytes;i++){
            const uint8_t value=data[i];
            if(!used){if(!value)continue;if(value!=4)return Protocol;needed=3;}
            if(used>=sizeof(event))return Protocol;
            event[used++]=value;
            if(used==3)needed=3+event[2];
            if(used!=needed)continue;
            reply.events++;
            if(event[1]==0x0e && event[2]>=4 && le16(event+4)==opcode){
                reply.status=event[6];reply.length=event[2]-4;
                for(size_t at=0;at<reply.length;at++)reply.payload[at]=event[7+at];
                reply.complete=true;
            }else if(event[1]==0x0f && event[2]==4 && le16(event+5)==opcode){
                if(event[3]){reply.status=event[3];reply.complete=true;}
            }else reply.unmatched++;
            used=0;needed=3;
        }
        return reply.complete && reply.status?Protocol:Ok;
    }
};

inline Result decodeHciAggregate(const uint8_t *data,size_t bytes,uint16_t opcode,
                                 HciEvents &events,HciReply &reply){
    if(!data || bytes!=kReceiveBytes)return Invalid;
    reply.aggregates++;reply.validBytes=le32(data+bytes-8);reply.remainingBytes=le32(data+bytes-4);
    if(reply.validBytes>kReceivePacketBytes)return Protocol;
    size_t at=0,consumed=0;
    while(at+4<=kReceivePacketBytes && consumed<reply.validBytes){
        const uint32_t header=le32(data+at);reply.publicHeader=header;
        if(header&(1u<<23))break;
        const unsigned type=header>>28,subtype=(header>>24)&15;
        const size_t length=(header>>7)&0xffff,wire=length+((header&64)?2:0);
        if(!length || wire>kReceivePacketBytes-at-4)return Protocol;
        if(type!=15){
            if(length>reply.validBytes-consumed)return Protocol;
            consumed+=length;
            if(type==0 && subtype==5){
                Result r=events.consume(data+at+4,length,opcode,reply);
                if(r!=Ok || reply.complete)return r;
            }else reply.unmatched++;
        }
        at+=4+((wire+3)&~size_t(3));
    }
    return Ok;
}

template<class Host,class Gpio> Result hciCommand(Host &host,Gpio &gpio,uint16_t opcode,
                               const uint8_t *data,size_t bytes,HciReply &reply){
    uint8_t tx[kPacketBlock]={},rx[kReceiveBytes]={};size_t used=0;HciEvents events;
    Result r=encodeHciCommand(tx,sizeof(tx),opcode,data,bytes,used);if(r!=Ok)return r;
    r=host.packets(true,tx,used);if(r!=Ok)return r;
    // The FIFO carries Wi-Fi and BT traffic together. Unrelated valid frames
    // consume time, not a protocol-error budget; the loop remains bounded.
    for(unsigned elapsed=0;elapsed<10000;elapsed++){
        if(gpio.read32(6*0x80)&1u){
            r=host.packets(false,rx,sizeof(rx));if(r!=Ok)return r;
            r=decodeHciAggregate(rx,sizeof(rx),opcode,events,reply);
            if(r!=Ok || reply.complete)return r;
        }
        gpio.delay(1000);
    }
    return Timeout;
}
}
