// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdBluetoothStream.h"
#include <cstdio>
#include <cstring>
using namespace SprdWcn;
int main() {
    bool pass=true;H4Stream tx,rx;unsigned sent=0,received=0;
#define CHECK(condition) do { if(!(condition)) { std::printf("line %u: %s\n", __LINE__, #condition); pass=false; } } while(0)
    uint8_t packet[kH4TransmitBytes]={};size_t used=0;
    auto send=[&](const uint8_t *p,size_t n){
        sent++;Result r=encodeH4Packet(p,n,packet,sizeof(packet),used);
        CHECK(r==Ok && used%840==0 && (le32(packet)>>24)==3 && !memcmp(packet+4,p,n));
        return r;
    };
    const uint8_t reset[]={1,3,12,0};
    CHECK(tx.consume(reset,2,false,send)==Ok && !sent);
    CHECK(tx.consume(reset+2,2,false,send)==Ok && sent==1 && used==840 &&
          ((le32(packet)>>7)&0xffff)==8 && le32(packet+12)==0x800000);
    uint8_t acl[4101]={2,1,0,0,16};for(unsigned i=5;i<sizeof(acl);i++)acl[i]=(uint8_t)i;
    for(size_t at=0;at<sizeof(acl);) {
        size_t n=sizeof(acl)-at;if(n>271)n=271;
        CHECK(tx.consume(acl+at,n,false,send)==Ok);at+=n;
    }
    CHECK(sent==2 && used==4200 && tx.used==0);
    const uint8_t complete[]={4,14,4,1,3,12,0,0,0,0};
    auto receive=[&](const uint8_t *p,size_t n){received++;CHECK(n==7 && !memcmp(p,complete,7));return Ok;};
    for(unsigned i=0;i<sizeof(complete);i++)CHECK(rx.consume(complete+i,1,true,receive)==Ok);
    CHECK(received==1 && rx.padding==3 && rx.used==0);
    const uint8_t impossible[]={2,1,0,255,255};
    CHECK(rx.consume(impossible,sizeof(impossible),true,receive)==Protocol);
    rx.reset();CHECK(rx.consume(reset,sizeof(reset),true,receive)==Protocol);
    tx.reset();CHECK(tx.consume(complete,7,false,send)==Protocol);
    tx.reset();auto blocked=[](const uint8_t *,size_t){return Busy;};
    CHECK(tx.consume(reset,sizeof(reset),false,blocked)==Busy);
    CHECK(tx.used==4);tx.reset();CHECK(tx.used==0);
    printf("%s: original H4 partial command/ACL/event, SDIO3 padding, frame limit, direction and backpressure\n",pass?"PASS":"FAIL");
    return pass?0:1;
}
