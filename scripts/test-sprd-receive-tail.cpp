// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdWcnReceive.h"
#include <cstdio>
#include <cstring>
using namespace SprdWcn;
struct Gpio {uint32_t value=1;uint32_t read32(unsigned){return value;}};
struct Packet {uint32_t valid,remaining;};
struct Host {
    Packet entries[10]={};unsigned count=0,reads=0;
    Result packets(bool write,uint8_t *out,size_t bytes) {
        if(write || bytes!=kReceiveBytes || reads>=count)return Invalid;
        Packet p=entries[reads++];memset(out,0,bytes);
        put32(out+bytes-8,p.valid);put32(out+bytes-4,p.remaining);return Ok;
    }
};
int main() {
    bool pass=true;
    {Host h;Gpio g;ReceiveState s;unsigned n=9;h.entries[0]={0,0};h.count=1;
     pass&=receiveSlice(h,g,s,n)==Ok && h.reads==1 && n==0 && s.empty==1 && !s.pending;}
    {Host h;Gpio g;ReceiveState s;unsigned n=0;h.entries[0]={40,0};h.count=1;
     pass&=receiveSlice(h,g,s,n)==Ok && h.reads==1 && n==1 && !s.pending;}
    {Host h;Gpio g;ReceiveState s;unsigned n=0;h.count=9;
     for(unsigned i=0;i<9;i++)h.entries[i]={40,i==8?0u:1u};
     pass&=receiveSlice(h,g,s,n)==Ok && h.reads==8 && n==8 && s.pending;
     g.value=0;pass&=receiveSlice(h,g,s,n)==Ok && h.reads==9 && n==1 && !s.pending;}
    {Host h;Gpio g;ReceiveState s;unsigned n=0;h.entries[0]={1681,0};h.count=1;
     pass&=receiveSlice(h,g,s,n)==Protocol && h.reads==1 && n==0;}
    {Host h;Gpio g;ReceiveState s;unsigned n=9;g.value=0;
     pass&=receiveSlice(h,g,s,n)==Ok && h.reads==0 && n==0;}
    std::printf("%s: RX tail stops empty/terminal batches, retains budget continuation, rejects malformed tail\n",pass?"PASS":"FAIL");
    return pass?0:1;
}
