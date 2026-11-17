// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdPwm.h"
#include "../drivers/SprdPlatform/FactoryBacklight.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
struct Registers {
    uint32_t words[0x80/4]={};bool refuse=false,clearTransient=false,corruptDivider=false;
    uint32_t activeDuty=81,activePrescale=0x8003;unsigned commits=0;
    std::vector<unsigned> writes;
    uint32_t read32(unsigned offset){assert(offset<sizeof(words));return words[offset/4];}
    void write32(unsigned offset,uint32_t value){
        assert(offset<sizeof(words));writes.push_back(offset);if(refuse)return;
        words[offset/4]=value;
        if(offset==0x58 && !value && clearTransient)words[0x40/4]&=~0x8000u;
        if(offset==0x40) {
            if(corruptDivider)words[offset/4]^=0x1000;
            activeDuty=words[0x48/4];activePrescale=words[0x40/4];commits++;
        }
    }
};
int main() {
    assert(SprdBus::backlightDuty(0)==0 && SprdBus::backlightDuty(1)==1 && SprdBus::backlightDuty(255)==255);
    for(unsigned level=1;level<=255;level++) {
        unsigned duty=SprdBus::backlightDuty(level);
        assert(duty>0 && duty<=255 && duty>=SprdBus::backlightDuty(level-1));
    }
    Registers io;for(unsigned i=0;i<32;i++)io.words[i]=0x12340000+i;
    io.words[0x40/4]=0x8003;io.words[0x44/4]=255;io.words[0x48/4]=81;io.words[0x58/4]=1;
    uint32_t baseline[32];std::memcpy(baseline,io.words,sizeof baseline);
    SprdBus::Pwm<Registers> pwm(io,2);SprdBus::PwmState state;
    assert(pwm.read(state)==SprdBus::Ok && state.duty==81);
    size_t initialWrites=io.writes.size();
    assert(pwm.duty(80,true)==SprdBus::Protocol && io.writes.size()==initialWrites);
    assert(pwm.configureFactory(32768,75000)==SprdBus::Invalid && io.writes.size()==initialWrites);
    assert(pwm.configureFactory(26000000,75000)==SprdBus::Ok);
    assert((io.writes==std::vector<unsigned>{0x44,0x48,0x50,0x54,0x40}));
    assert(io.activePrescale==6 && io.activeDuty==81 && io.commits==1);
    assert(io.words[0x50/4]==0xffff && io.words[0x54/4]==0xffff);
    for(unsigned n=0;n<256;n++){
        size_t start=io.writes.size();
        assert(pwm.duty(n,true)==SprdBus::Ok);
        assert(io.activeDuty==n && io.activePrescale==6);
        if(start!=io.writes.size())assert(io.writes.back()==0x40);
        for(unsigned i=0;i<32;i++)if(i!=0x40/4 && i!=0x44/4 && i!=0x48/4 && i!=0x50/4 && i!=0x54/4 && i!=0x58/4)assert(io.words[i]==baseline[i]);
    }
    size_t unchanged=io.writes.size();assert(pwm.duty(255,true)==SprdBus::Ok && io.writes.size()==unchanged);
    assert(pwm.duty(0,false)==SprdBus::Ok && io.words[0x58/4]==0);
    assert(pwm.duty(256,true)==SprdBus::Invalid);
    assert(pwm.restore(state)==SprdBus::Ok);
    for(unsigned i=0;i<32;i++)assert(io.words[i]==baseline[i]);
    io.words[0x40/4]=65536;assert(pwm.read(state)==SprdBus::Protocol);io.words[0x40/4]=0x8003;
    assert(pwm.configureFactory(26000000,75000)==SprdBus::Ok);
    io.refuse=true;assert(pwm.duty(82,true)==SprdBus::Protocol);
    assert(pwm.diagnostic().phase==3 && pwm.diagnostic().before.duty==81 && pwm.diagnostic().after.duty==81);
    io.refuse=false;
    io.words[0x40/4]=0x8006;io.clearTransient=true;
    assert(pwm.duty(0,false)==SprdBus::Ok && pwm.diagnostic().phase==4 && io.words[0x40/4]==6);
    assert(pwm.duty(52,true)==SprdBus::Ok && io.words[0x58/4]==1);
    io.corruptDivider=true;assert(pwm.duty(53,true)==SprdBus::Protocol);io.corruptDivider=false;
    io.words[0x44/4]=0;size_t before=io.writes.size();
    assert(pwm.duty(80,true)==SprdBus::Protocol && io.writes.size()==before);
    SprdBus::Pwm<Registers> bad(io,4);assert(bad.read(state)==SprdBus::Invalid);
    std::puts("PASS: exact original75us PWM2 tuple and PRESCALE-last commit, 256 levels, no duplicate ENABLE writes, period/pattern/sibling preservation, full baseline restoration, off/resume, torn-write rejection and nonzero factory curve");
}
