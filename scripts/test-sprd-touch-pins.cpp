// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdTouchPins.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
struct Registers {
    uint32_t data[0x1000/4]={};bool refuse=false;unsigned elapsed=0;
    std::vector<std::pair<unsigned,uint32_t>> writes;
    uint32_t read32(unsigned offset){assert(offset<sizeof data);return data[offset/4];}
    void write32(unsigned offset,uint32_t value){assert(offset<sizeof data);writes.emplace_back(offset,value);if(!refuse)data[offset/4]=value;}
    void delay(unsigned us){elapsed+=us;}
};
int main() {
    Registers pins,gpio;for(unsigned i=0;i<1024;i++){pins.data[i]=0x12340000+i;gpio.data[i]=0xabcdef00+i;}
    uint32_t beforePins[1024],beforeGpio[1024];std::memcpy(beforePins,pins.data,sizeof beforePins);std::memcpy(beforeGpio,gpio.data,sizeof beforeGpio);
    SprdBus::TouchPins<Registers,Registers> touch(pins,gpio);
    assert(touch.resetToAddress14()==SprdBus::Ok && pins.elapsed==78000);
    for(unsigned i=0;i<1024;i++) {
        if(i==0x2b8/4 || i==0x2bc/4)assert(pins.data[i]==((beforePins[i]&~0x30u)|0x30));
        else if(i==0x2c0/4 || i==0x2c4/4)assert(pins.data[i]==(beforePins[i]&~0x30u));
        else assert(pins.data[i]==beforePins[i]);
        if(i>=0x480/4 && i<=0x4a8/4)assert((gpio.data[i]&~3u)==(beforeGpio[i]&~3u));
        else assert(gpio.data[i]==beforeGpio[i]);
    }
    assert((gpio.data[0x480/4]&3)==2 && (gpio.data[0x488/4]&3)==2 && (gpio.data[0x4a8/4]&1)==1);
    assert(touch.resetToAddress14()==SprdBus::Invalid);
    touch.restore();assert(!std::memcmp(beforePins,pins.data,sizeof beforePins));assert(!std::memcmp(beforeGpio,gpio.data,sizeof beforeGpio));
    pins.refuse=true;assert(touch.resetToAddress14()==SprdBus::Protocol);
    assert(!std::memcmp(beforeGpio,gpio.data,sizeof beforeGpio));
    std::puts("PASS: factory78ms reset/address strap, exact pin offsets, sibling preservation, input release, readback refusal and rollback");
}
