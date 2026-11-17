// SPDX-License-Identifier: GPL-2.0-only
// Exact shipped1h190 marlin chip_power_on(), raw GPIO semantics and SC2730
// regulator_desc fields. GPIO_ACTIVE_LOW in DT is not an output inversion:
// the original uses gpiod_direction_output_raw() for CHIP_EN and RESET.
#pragma once
#include "SprdBus.h"
namespace SprdWcn {
using namespace SprdBus;
template<class IO,class PMIC> class Power {
    IO &gpio,&aon,&ap,&clock,&apClock;PMIC &pmic;
    static constexpr unsigned pmicRegs[8]={0x1998,0x1994,0x1920,0x191c,0x19d4,0x19d0,0x1ac8,0x1acc};
    static constexpr unsigned pmicMasks[8]={0xff,1,0x3f,1,0xff,1,0x1000,4};
    uint16_t savedPmic[8]={};unsigned savedCount=0;
    uint32_t savedGpio[2][4]={},savedAux=0,savedSdioClock=0,aonAdded=0,apAdded=0;
    bool pinsSaved=false,clocksSaved=false;
    static constexpr unsigned gpioRegs[4]={0,4,8,0x28};
    Result update(unsigned r,unsigned mask,unsigned value){
        Result e=pmic.update(r,(uint16_t)mask,(uint16_t)value);uint16_t got=0;
        if(e==Ok)e=pmic.read(r,got);
        return e!=Ok?e:((got&mask)==(value&mask)?Ok:Protocol);
    }
    bool raw(unsigned pin,bool high){
        unsigned bank=(pin/16)*0x80;uint32_t mask=1u<<(pin%16);
        gpio.write32(bank+4,gpio.read32(bank+4)|mask);
        gpio.write32(bank,(gpio.read32(bank)&~mask)|(high?mask:0));
        gpio.write32(bank+8,gpio.read32(bank+8)|mask);
        return (gpio.read32(bank+8)&mask) && !!(gpio.read32(bank)&mask)==high;
    }
public:
    explicit Power(IO &g,IO &a,IO &p,IO &c,IO &ac,PMIC &pm):gpio(g),aon(a),ap(p),clock(c),apClock(ac),pmic(pm){}
    Result enable(){
        if(savedCount || clocksSaved)return Invalid;
        for(unsigned i=0;i<8;i++){
            Result r=pmic.read(pmicRegs[i],savedPmic[i]);if(r!=Ok)return r;savedCount++;
        }
        for(unsigned i=0;i<2;i++)for(unsigned j=0;j<4;j++)savedGpio[i][j]=gpio.read32(((i?97:143)/16)*0x80+gpioRegs[j]);
        pinsSaved=true;savedAux=clock.read32(0x228);savedSdioClock=apClock.read32(0x88);clocksSaved=true;
        Result r=update(0x1998,0xff,60);if(r!=Ok)return r; // DCXO18=1.8V.
        if((r=update(0x1994,1,0))!=Ok)return r;
        clock.write32(0x228,savedAux&~0xf0fu); // AUX0 ext32k, divide1.
        aonAdded=(~aon.read32(0))&0x10;if(aonAdded)aon.write32(0x1000,aonAdded);
        if((clock.read32(0x228)&0xf0f) || !(aon.read32(0)&0x10))return Protocol;
        if(!raw(143,false))return Protocol;gpio.delay(1000);
        if(!raw(143,true))return Protocol;gpio.delay(1000);gpio.delay(4000);
        if(!raw(97,true))return Protocol;gpio.delay(4000);
        if((r=update(0x1920,0x3f,20))!=Ok || (r=update(0x191c,1,0))!=Ok)return r;
        if((r=update(0x1ac8,0x1000,0x1000))!=Ok)return r;gpio.delay(1000);gpio.delay(50);
        gpio.delay(4000);
        if((r=update(0x19d4,0xff,210))!=Ok || (r=update(0x19d0,1,0))!=Ok)return r;
        if((r=update(0x1acc,4,4))!=Ok)return r;gpio.delay(1000);
        apClock.write32(0x88,(savedSdioClock&~7u)|1u); // Known ext26m source.
        apAdded=(~ap.read32(0))&(1u<<23);if(apAdded)ap.write32(0x1000,apAdded);
        return (apClock.read32(0x88)&7)==1 && (ap.read32(0)&(1u<<23))?Ok:Protocol;
    }
    Result restore(){
        Result first=Ok;
        if(pinsSaved){
            // Remove card power requests before releasing rails and clocks.
            if(!raw(143,false))first=Protocol;
            if(!raw(97,false) && first==Ok)first=Protocol;
        }
        if(clocksSaved){
            if(apAdded)ap.write32(0x2000,apAdded);
            apClock.write32(0x88,(apClock.read32(0x88)&~7u)|(savedSdioClock&7));
            if(aonAdded)aon.write32(0x2000,aonAdded);
            clock.write32(0x228,(clock.read32(0x228)&~0xf0fu)|(savedAux&0xf0f));
        }
        while(savedCount){unsigned i=--savedCount;Result r=update(pmicRegs[i],pmicMasks[i],savedPmic[i]);if(first==Ok && r!=Ok)first=r;}
        if(pinsSaved)for(unsigned i=0;i<2;i++){
            unsigned pin=i?97:143,bank=(pin/16)*0x80,mask=1u<<(pin%16);
            for(unsigned j=0;j<4;j++)gpio.write32(bank+gpioRegs[j],(gpio.read32(bank+gpioRegs[j])&~mask)|(savedGpio[i][j]&mask));
        }
        pinsSaved=clocksSaved=false;aonAdded=apAdded=0;return first;
    }
};
}
