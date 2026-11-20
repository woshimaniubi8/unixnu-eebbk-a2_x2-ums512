// SPDX-License-Identifier: GPL-2.0-only
// Shipped mali_gondul.ko SharkL5Pro domain/clock ordering; standard Bifrost
// register definitions from Linux panfrost_regs.h v6.6. No Adreno registers.
#pragma once
#include "SprdBus.h"
namespace SprdMali {
using namespace SprdBus;
struct Identity {uint32_t id=0,status=0,shader=0,tiler=0,l2=0,as=0,js=0,irq=0,resetPolls=0;};
inline bool factorySupplyReady(uint16_t pd,uint16_t selector){
    // The original lowest OPP is700mV. A successful PMIC transaction alone
    // does not qualify a lower, uncharacterized voltage for GPU access.
    return !(pd&8) && (selector&0x1ff)>=224;
}
struct ClockProfile {
    uint32_t coreMHz=0,memoryMHz=0,systemMHz=0;
    uint32_t coreWord=0,memoryWord=0,systemWord=0;
    uint32_t minimumMicrovolts=0;
};
inline bool clockProfile(uint32_t coreMHz,ClockProfile &out){
    // Linux ums512-clk.c + composite.h: MUX [6:4], divider [10:8], gate [0].
    // Parents 0/1/4 are ext-26m, twpll-384m and twpll-768m. SYS divides MEM.
    // Original board DT OPPs: 384 MHz / 700000 uV; 768 MHz / 800000 uV.
    // This driver never changes PMIC voltage to make a clock profile qualify.
    if(coreMHz==26){out={26,26,26,1,1,1,700000};return true;}
    if(coreMHz==384){out={384,384,192,0x11,0x11,0x11,700000};return true;}
    if(coreMHz==768){out={768,768,384,0x41,0x41,0x11,800000};return true;}
    return false;
}
inline bool profileSupplyReady(const ClockProfile &profile,uint32_t measuredMicrovolts){
    return profile.minimumMicrovolts>=700000 && measuredMicrovolts>=profile.minimumMicrovolts;
}
template<class IO> class Probe {
    IO &pmu,&aon,&clock,&gpu;
    uint32_t oldPower=0,addedGate=0,oldClock[3]={},oldIrq=0;
    bool powerSaved=false,clockSaved=false,irqSaved=false;
public:
    explicit Probe(IO &p,IO &a,IO &c,IO &g):pmu(p),aon(a),clock(c),gpu(g){}
    Result run(Identity &out){
        if(powerSaved)return Invalid;
        oldPower=pmu.read32(0x30);powerSaved=true;
        pmu.write32(0x30,oldPower&~(1u<<25));pmu.delay(300);
        if(pmu.read32(0x30)&(1u<<25))return Protocol;
        addedGate=(~aon.read32(0))&(1u<<11);
        if(addedGate)aon.write32(0x1000,addedGate);
        if(!(aon.read32(0)&(1u<<11)))return Protocol;
        for(unsigned i=0;i<3;i++)oldClock[i]=clock.read32(4+4*i);
        clockSaved=true;
        // Register identity/reset needs no PLL, job buffer or DVFS vote.
        // ext26m is a real original clock parent, not a fabricated rate.
        for(unsigned i=0;i<2;i++)clock.write32(4+4*i,(oldClock[i]&~0x771u)|1u);
        clock.write32(0xc,(oldClock[2]&~0x71u)|1u);clock.delay(400);
        for(unsigned i=0;i<3;i++)if(!(clock.read32(4+4*i)&1))return Protocol;
        out.id=gpu.read32(0);out.status=gpu.read32(0x34);
        if(!out.id || out.id==0xffffffff)return Protocol;
        oldIrq=gpu.read32(0x28);irqSaved=true;
        gpu.write32(0x28,0);gpu.write32(0x24,0x100);gpu.write32(0x30,1);
        for(unsigned i=0;i<100000;i++){
            out.irq=gpu.read32(0x20);out.resetPolls=i;
            if(out.irq&0x81)return Protocol;
            if(out.irq&0x100){
                gpu.write32(0x24,0x100);
                out.shader=gpu.read32(0x100);out.tiler=gpu.read32(0x110);out.l2=gpu.read32(0x120);
                out.as=gpu.read32(0x18);out.js=gpu.read32(0x1c);
                return out.shader && out.tiler && out.l2 && out.as && out.js?Ok:Protocol;
            }gpu.delay(1);
        }return Timeout;
    }
    void restore(){
        if(irqSaved)gpu.write32(0x28,oldIrq);
        if(clockSaved)for(unsigned i=0;i<3;i++){
            unsigned mask=i==2?0x71:0x771;
            clock.write32(4+4*i,(clock.read32(4+4*i)&~mask)|(oldClock[i]&mask));
        }
        if(addedGate)aon.write32(0x2000,addedGate);
        if(powerSaved)pmu.write32(0x30,(pmu.read32(0x30)&~(1u<<25))|(oldPower&(1u<<25)));
        powerSaved=clockSaved=irqSaved=false;addedGate=0;
    }
};
}
