// SPDX-License-Identifier: GPL-2.0-only
// Factory 1h190 GPIO145 RESET, GPIO144 INT, and SHARKL5PRO pinctrl. Register
// offsets/sequence are checked against the shipped pinctrl and gt9xx code.
#pragma once
#include "SprdBus.h"
namespace SprdBus {
template<class Pins,class Gpio> class TouchPins {
    Pins &pins;Gpio &gpio;uint32_t savedPins[4]={},savedGpio[4]={};bool owned=false;
    static constexpr unsigned pin[4]={0x2b8,0x2bc,0x2c0,0x2c4};
    static constexpr unsigned bank=9*0x80,reg[4]={0,4,8,0x28};
    void bits(unsigned off,uint32_t mask,uint32_t value) {gpio.write32(bank+off,(gpio.read32(bank+off)&~mask)|(value&mask));}
    void output(unsigned mask,bool high) {
        bits(4,mask,mask);bits(8,mask,mask);bits(0x28,mask,0);bits(0,mask,high?mask:0);
    }
public:
    TouchPins(Pins &p,Gpio &g):pins(p),gpio(g) {}
    Result resetToAddress14() {
        if(owned)return Invalid;
        for(unsigned i=0;i<4;i++){savedPins[i]=pins.read32(pin[i]);savedGpio[i]=gpio.read32(bank+reg[i]);}
        owned=true;
        // func1 on I2C SDA3/SCL3, func4 on INT/RESET GPIO pads. Preserve
        // pull, drive strength, voltage-domain and sleep configuration bits.
        for(unsigned i=0;i<4;i++)pins.write32(pin[i],(savedPins[i]&~0x30u)|(i<2?0x30u:0));
        for(unsigned i=0;i<4;i++)if((pins.read32(pin[i])&0x30)!=(i<2?0x30u:0u)){restore();return Protocol;}
        output(2,false);pins.delay(20000);
        // gt9xx: INT high while RESET rises selects address0x14.
        output(1,true);pins.delay(2000);output(2,true);pins.delay(6000);
        output(1,false);pins.delay(50000);
        bits(8,1,0);bits(0x28,1,1);
        return Ok;
    }
    void restore() {
        if(!owned)return;
        for(unsigned i=0;i<4;i++)bits(reg[i],3,savedGpio[i]);
        for(unsigned i=0;i<4;i++)pins.write32(pin[i],(pins.read32(pin[i])&~0x30u)|(savedPins[i]&0x30));
        owned=false;
    }
};
}
