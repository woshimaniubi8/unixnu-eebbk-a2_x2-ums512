// SPDX-License-Identifier: GPL-2.0-only
// H4 framing follows the original sprdbt_tty sitm/parse_frame contract.
// Bluetooth Core HCI packet lengths, with original SDIO channel3/17 framing.
#pragma once
#include "SprdWcnPackets.h"
namespace SprdWcn {
constexpr size_t kH4Maximum=4101,kH4TransmitBytes=5040;
struct H4Stream {
    uint8_t data[kH4Maximum]={};size_t used=0,needed=0;
    bool headerComplete=false;
    unsigned frames=0,padding=0;
    void reset(){used=needed=0;headerComplete=false;}
    template<class Sink> Result consume(const uint8_t *input,size_t bytes,bool receive,Sink sink) {
        if(!input && bytes)return Invalid;
        for(size_t i=0;i<bytes;i++) {
            const uint8_t v=input[i];
            if(!used) {
                // SDIO inserts alignment bytes between H4 packets, never
                // inside the length-delimited command/event/ACL body.
                if(!v){padding++;continue;}
                if(v==1 && !receive)needed=4;
                else if(v==2)needed=5;
                else if(v==3)needed=4;
                else if(v==4 && receive)needed=3;
                else return Protocol;
            }
            if(used>=sizeof(data))return Protocol;
            data[used++]=v;
            if(!headerComplete && used==needed) {
                size_t total=needed;
                if(data[0]==1 || data[0]==3)total+=data[3];
                else if(data[0]==2)total+=le16(data+3);
                else total+=data[2];
                // Once the header is complete, the payload is opaque.
                if(total>sizeof(data))return Protocol;
                headerComplete=true;
                if(total!=needed){needed=total;continue;}
            }
            if(used==needed) {
                Result result=sink(data,used);
                if(result!=Ok)return result;
                frames++;reset();
            }
        }
        return Ok;
    }
};
inline Result encodeH4Packet(const uint8_t *h4,size_t bytes,uint8_t *out,size_t capacity,size_t &used) {
    used=0;if(!h4 || !bytes || bytes>kH4Maximum || !out)return Invalid;
    // The vendor sitm path adds a full alignment word when already aligned.
    // This is the same rule qualified by the factory HCI initialization.
    const size_t wire=bytes+4-bytes%4,eof=4+wire,padded=((eof+4+839)/840)*840;
    if(padded>capacity)return Invalid;
    for(size_t i=0;i<padded;i++)out[i]=0;
    put32(out,(3u<<24)|(uint32_t)(wire<<7));
    for(size_t i=0;i<bytes;i++)out[4+i]=h4[i];
    put32(out+eof,1u<<23);used=padded;return Ok;
}
}
