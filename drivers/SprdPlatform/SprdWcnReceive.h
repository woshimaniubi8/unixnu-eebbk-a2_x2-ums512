// SPDX-License-Identifier: GPL-2.0-only
// Original BSP sdiohal_rx.c: continue a batch from its DTBS remaining count,
// then return to the interrupt wait. A high GPIO is not a remaining-packet count.
#pragma once
#include "SprdWcnPackets.h"
namespace SprdWcn {
struct ReceiveState {
    bool pending=false;
    unsigned batches=0,empty=0;
    uint32_t valid=0,remaining=0,header=0;
};
template<class Host,class Gpio>
inline Result receiveSlice(Host &host,Gpio &gpio,ReceiveState &state,unsigned &packets,
                           PacketObserver observer={}) {
    packets=0;
    if(!state.pending && !(gpio.read32(kPacketGpioBank)&1u))return Ok;
    uint8_t bytes[kReceiveBytes]={};
    for(unsigned i=0;i<8;i++) {
        Result result=host.packets(false,bytes,sizeof(bytes));
        if(result!=Ok)return result;
        state.batches++;state.header=le32(bytes);
        state.valid=le32(bytes+sizeof(bytes)-8);
        state.remaining=le32(bytes+sizeof(bytes)-4);
        if(state.valid>kReceivePacketBytes)return Protocol;
        if(!state.valid) {
            state.empty++;state.pending=false;return Ok;
        }
        if(observer.packet){result=observeAggregate(bytes,sizeof(bytes),observer);if(result!=Ok)return result;}
        packets++;state.pending=state.remaining!=0;
        if(!state.pending)return Ok;
    }
    // The work-loop budget is only a scheduling boundary: a real continuation
    // survives it even if the GPIO changes before the next timer turn.
    return Ok;
}
}
