// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdPwm.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
struct Registers {
    uint32_t words[0x80/4]={};bool refuse=false,clearTransient=false,corruptDivider=false;
    std::vector<unsigned> writes;
    uint32_t read32(unsigned offset){assert(offset<sizeof(words));return words[offset/4];}
    void write32(unsigned offset,uint32_t value){
        assert(offset<sizeof(words));writes.push_back(offset);if(refuse)return;
        words[offset/4]=value;
        if(offset==0x58 && !value && clearTransient)words[0x40/4]&=~0x8000u;
        if(offset==0x48 && corruptDivider)words[0x40/4]^=0x1000;
    }
};
int main() {
    Registers io;for(unsigned i=0;i<32;i++)io.words[i]=0x12340000+i;
    io.words[0x40/4]=0x8003;io.words[0x44/4]=255;io.words[0x48/4]=81;io.words[0x58/4]=1;
    uint32_t baseline[32];std::memcpy(baseline,io.words,sizeof baseline);
    SprdBus::Pwm<Registers> pwm(io,2);SprdBus::PwmState state;
    assert(pwm.read(state)==SprdBus::Ok && state.duty==81);
    for(unsigned n=0;n<256;n++){
        assert(pwm.duty(n,true)==SprdBus::Ok);
        for(unsigned i=0;i<32;i++)if(i!=0x48/4 && i!=0x58/4)assert(io.words[i]==baseline[i]);
    }
    assert(pwm.duty(0,false)==SprdBus::Ok && io.words[0x58/4]==0);
    assert(pwm.duty(256,true)==SprdBus::Invalid);
    assert(pwm.restore(state)==SprdBus::Ok);
    assert(io.words[0x40/4]==(baseline[0x40/4]&0x7fff));
    for(unsigned i=0;i<32;i++)if(i!=0x40/4)assert(io.words[i]==baseline[i]);
    io.words[0x40/4]=65536;assert(pwm.read(state)==SprdBus::Protocol);io.words[0x40/4]=0x8003;
    io.refuse=true;assert(pwm.duty(82,true)==SprdBus::Protocol);
    assert(pwm.diagnostic().phase==3 && pwm.diagnostic().before.duty==81 && pwm.diagnostic().after.duty==81);
    io.refuse=false;
    io.words[0x40/4]=0x8003;io.clearTransient=true;
    assert(pwm.duty(0,false)==SprdBus::Ok && pwm.diagnostic().phase==4 && io.words[0x40/4]==3);
    assert(pwm.duty(52,true)==SprdBus::Ok && io.words[0x58/4]==1);
    io.corruptDivider=true;assert(pwm.duty(53,true)==SprdBus::Protocol);io.corruptDivider=false;
    io.words[0x44/4]=0;size_t before=io.writes.size();
    assert(pwm.duty(80,true)==SprdBus::Protocol && io.writes.size()==before);
    SprdBus::Pwm<Registers> bad(io,4);assert(bad.read(state)==SprdBus::Invalid);
    std::puts("PASS: PWM2 256 levels, period and sibling preservation, off/resume, readback failure, invalid takeover, stable period restoration and bit15 transition without masking divider faults");
}
