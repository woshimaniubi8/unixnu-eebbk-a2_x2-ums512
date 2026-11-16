// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdBus.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
using namespace SprdBus;
struct AdiModel {
    uint32_t command=0,data=0;unsigned waits=0,writes=0;
    bool busy=false,badEcho=false,full=false,draining=false;
    uint32_t read32(unsigned off) {
        if(off==0x2c)return busy?0x80000000:((((command>>2)&0x7fff)^(badEcho?1:0))<<16)|data;
        if(off==0x30)return (draining?0:1u<<10)|(full?1u<<11:0);
        assert(false);return 0;
    }
    void write32(unsigned off,uint32_t v) {writes++;if(off==0x28)command=v;else {assert(off>=0x20000 && off<0x40000);data=v;}}
    void delay(unsigned us){waits+=us;}
};
struct I2cModel {
    uint32_t regs[16]={};unsigned elapsed=0,started=0,delayCalls=0;
    uint8_t memory[65536]={};uint16_t pointer=0;unsigned addr=0,width=2,remaining=0;
    bool nack=false,timeout=false;std::vector<uint8_t> tx;
    std::vector<std::pair<unsigned,unsigned>> messages;
    uint32_t read32(unsigned off){if(off==0x14 && started && !timeout && remaining && (regs[0]&2)){if(++elapsed>3)return 4|(nack?2:0);}return regs[off/4];}
    void write32(unsigned off,uint32_t v) {
        if(off==0x2c){tx.clear();regs[off/4]=v;return;}
        regs[off/4]=v;
        if(off==0 && (v&1)) {
            started++;elapsed=0;remaining=regs[2];addr=regs[1]>>1;messages.emplace_back(addr,v);
            if(!(v&8) && !nack) {
                assert(tx.size()==remaining && tx.size()>=width);
                pointer=width==2?(uint16_t)((tx[0]<<8)|tx[1]):tx[0];
                for(unsigned i=width;i<tx.size();i++)memory[pointer++]=tx[i];
            }
        }
        if(off==0x14 && !(v&4))remaining=0;
    }
    uint8_t read8(unsigned off){assert(off==12 && remaining);remaining--;return memory[pointer++];}
    void write8(unsigned off,uint8_t v){assert(off==16);tx.push_back(v);}
    void delay(unsigned){delayCalls++;}
};
int main() {
    AdiModel m;Adi<AdiModel> adi(m);uint16_t value=0xdead;
    m.data=0x1234;assert(adi.read(0x280,value)==Ok && value==0x1234 && m.command==0x32120280);
    m.badEcho=true;assert(adi.read(0x280,value)==Protocol && value==0x1234);m.badEcho=false;
    m.busy=true;assert(adi.read(0x280,value)==Timeout && m.waits==2000);m.busy=false;
    unsigned w=m.writes;assert(adi.read(3,value)==Invalid && adi.write(0x20000,4)==Invalid && m.writes==w);
    assert(adi.update(0x284,0x1a,0x1a)==Ok && m.data==0x123e);
    m.full=true;assert(adi.write(0x284,1)==Timeout);m.full=false;
    m.draining=true;assert(adi.write(0x284,1)==Timeout);m.draining=false;
    I2cModel i;I2c<I2cModel> bus(i);bus.configure26000kHz400kHz();
    assert(i.regs[8]==((12u<<16)|18u) && i.regs[10]==15 && (i.regs[0]&6)==6);
    for(unsigned n=0;n<80;n++)i.memory[0x8140+n]=(uint8_t)(n+1);
    uint8_t result[80]={};assert(bus.readRegister(0x14,0x8140,2,result,sizeof(result))==Ok);
    assert(i.messages.size()==20);
    for(unsigned n=0;n<80;n++)assert(result[n]==n+1);
    for(unsigned n=0;n<20;n+=2){assert(i.messages[n].second&(1u<<20));assert((i.messages[n+1].second&((1u<<20)|8))==8);}
    uint8_t zero=0;assert(bus.writeRegister(0x14,0x814e,2,&zero,1)==Ok && i.memory[0x814e]==0);
    i.nack=true;assert(bus.readRegister(0x14,0x8140,2,result,1)==Nack);i.nack=false;
    i.timeout=true;unsigned before=i.delayCalls;assert(bus.readRegister(0x14,0x8140,2,result,1)==Timeout && i.delayCalls-before==5000);i.timeout=false;
    // Replay the M102 register state: EN without INT_EN cannot complete.
    i.regs[0]&=~2u;before=i.delayCalls;
    assert(bus.readRegister(0x14,0x8140,2,result,1)==Timeout && i.delayCalls-before==5000);
    bus.configure26000kHz400kHz();
    assert(bus.readRegister(0x14,0x8140,2,result,1)==Ok);
    size_t messages=i.messages.size();
    assert(bus.readRegister(0x14,255,1,result,2)==Invalid && i.messages.size()==messages);
    assert(bus.writeRegister(0x14,0,1,&zero,(size_t)-1)==Invalid);
    uint8_t goodix[80]={1,0x2c,1,0x90,1};Frame f={};
    assert(goodixFrame(goodix,8,1,1200,1920,f)==Ok && f.count==1 && f.contacts[0].x==300 && f.contacts[0].y==400);
    std::memcpy(goodix+8,goodix,8);assert(goodixFrame(goodix,16,2,1200,1920,f)==Protocol);
    assert(goodixFrame(goodix,7,1,1200,1920,f)==Invalid);
    uint8_t ft[63]={};std::memset(ft+3,0xff,60);ft[2]=1;ft[3]=0;ft[4]=100;ft[5]=0x20;ft[6]=200;
    assert(focaltechFrame(ft,63,1280,1752,f)==Ok && f.count==1 && f.contacts[0].id==2);
    ft[2]=2;assert(focaltechFrame(ft,63,1280,1752,f)==Protocol);
    Debounce d;assert(!d.sample(2));assert(!d.sample(0));assert(!d.sample(2));assert(!d.sample(2));assert(d.sample(2) && d.stable==2);
    assert(!d.sample(0));assert(!d.sample(0));assert(d.sample(0) && d.stable==0);
    uint32_t random=1;unsigned cases=0;
    for(unsigned n=0;n<100000;n++) {
        for(auto &v:goodix){random=random*1664525+1013904223;v=(uint8_t)(random>>24);}
        unsigned count=random%17;Frame candidate={};
        Result r=goodixFrame(goodix,sizeof(goodix),count,1200,1920,candidate);
        if(r==Ok){assert(candidate.count<=10);for(unsigned j=0;j<candidate.count;j++)assert(candidate.contacts[j].x<1200 && candidate.contacts[j].y<1920);}
        r=focaltechFrame(goodix,63,1280,1752,candidate);if(r==Ok)assert(candidate.count<=10);cases+=2;
    }
    std::printf("PASS: ADIv3 echo/timeout/masked update, I2C repeated-start/chunking/NACK/deadline/bounds, debounce press/release, touch parsers; fuzz cases=%u\n",cases);
}
