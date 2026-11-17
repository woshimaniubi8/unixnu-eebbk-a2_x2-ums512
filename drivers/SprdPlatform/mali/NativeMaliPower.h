// SPDX-License-Identifier: GPL-2.0-only
// EEBBK adapter for the pinned Mali-G52 register core. Domain/rail programming
// stays with SprdMali::Probe. This class only qualifies those live resources.
#pragma once
#include <IOKit/IOTypes.h>
#include <stdint.h>
class NativeMaliPower final {
    volatile uint8_t *pmu_,*aon_,*clock_;
    static uint32_t read(volatile uint8_t *p,unsigned at){
        uint32_t value=*(volatile uint32_t *)(p+at);
        __asm__ volatile("dsb ld":::"memory");return value;
    }
public:
    NativeMaliPower(volatile uint8_t *p,volatile uint8_t *a,volatile uint8_t *c):pmu_(p),aon_(a),clock_(c){}
    bool prepared() const{return pmu_ && aon_ && clock_;}
    IOReturn gpuReadable() const{
        if(!prepared())return kIOReturnNotReady;
        if((read(pmu_,0x30)&(1u<<25)) || !(read(aon_,0)&(1u<<11)))return kIOReturnNotReady;
        for(unsigned i=0;i<3;i++)if(!(read(clock_,4+4*i)&1))return kIOReturnNotReady;
        return kIOReturnSuccess;
    }
};
