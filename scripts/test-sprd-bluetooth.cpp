// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdWcnBluetooth.h"
#include "../drivers/SprdPlatform/SprdWcnBluetoothData.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>
using namespace SprdWcn;
int main(){
    static_assert(sizeof(kFactoryBtPskey)==160 && sizeof(kFactoryBtRf)==252,"original INI ABI");
    for(unsigned i=20;i<26;i++)assert(kFactoryBtPskey[i]==0);
    uint8_t packet[840]={},params[255]={};size_t used=0;
    assert(encodeHciCommand(packet,sizeof(packet),0x0c03,nullptr,0,used)==Ok&&used==840);
    assert(le32(packet)==((3u<<24)|(8u<<7))&&packet[4]==1&&le16(packet+5)==0x0c03&&!packet[7]);
    assert(le32(packet+12)==(1u<<23));
    assert(encodeHciCommand(packet,sizeof(packet),0xfca0,params,160,used)==Ok);
    assert(le32(packet)==((3u<<24)|(168u<<7))&&packet[7]==160&&le32(packet+172)==(1u<<23));
    assert(encodeHciCommand(packet,sizeof(packet),0xfca2,params,252,used)==Ok&&le32(packet+264)==(1u<<23));
    assert(encodeHciCommand(packet,sizeof(packet),0xfca2,params,256,used)==Invalid&&!used);
    const uint8_t reset[]={4,0x0e,4,1,3,0x0c,0};
    for(size_t split=0;split<=sizeof(reset);split++){
        HciEvents h;HciReply r;
        assert(h.consume(reset,split,0x0c03,r)==Ok);
        if(split<sizeof(reset))assert(!r.complete);
        assert(h.consume(reset+split,sizeof(reset)-split,0x0c03,r)==Ok&&r.complete&&!r.status&&!r.length);
    }
    const uint8_t stale[]={0,0,4,0x0e,4,1,1,0x10,0,0,0};
    HciEvents h;HciReply r;
    assert(h.consume(stale,sizeof(stale),0x0c03,r)==Ok&&!r.complete&&r.unmatched==1);
    const uint8_t status[]={4,0x0f,4,0,1,3,0x0c};
    assert(h.consume(status,sizeof(status),0x0c03,r)==Ok&&!r.complete);
    assert(h.consume(reset,sizeof(reset),0x0c03,r)==Ok&&r.complete&&!r.status);
    const uint8_t denied[]={4,0x0f,4,0x0c,1,3,0x0c};
    HciEvents deniedH;HciReply deniedR;
    assert(deniedH.consume(denied,sizeof(denied),0x0c03,deniedR)==Protocol&&deniedR.complete&&deniedR.status==0x0c);
    uint8_t maxEvent[258]={4,0x0e,255,1,1,0x10,0};
    HciEvents maxH;HciReply maxR;
    assert(maxH.consume(maxEvent,sizeof(maxEvent),0x1001,maxR)==Ok&&maxR.complete&&maxR.length==251);
    uint8_t rx[2520]={};put32(rx,(5u<<24)|(sizeof(reset)<<7));std::copy(reset,reset+sizeof(reset),rx+4);
    put32(rx+12,1u<<23);put32(rx+2512,sizeof(reset));
    HciEvents aggregateH;HciReply aggregateR;
    assert(decodeHciAggregate(rx,sizeof(rx),0x0c03,aggregateH,aggregateR)==Ok&&aggregateR.complete&&aggregateR.aggregates==1);
    put32(rx+2512,3);assert(decodeHciAggregate(rx,sizeof(rx),0x0c03,aggregateH,aggregateR)==Protocol);
    put32(rx+2512,1681);assert(decodeHciAggregate(rx,sizeof(rx),0x0c03,aggregateH,aggregateR)==Protocol);
    assert(decodeHciAggregate(rx,1680,0x0c03,aggregateH,aggregateR)==Invalid);
    puts("PASS: factory H4 padding, partial events, opcode/status matching and original ADMA HCI channel geometry");
}
