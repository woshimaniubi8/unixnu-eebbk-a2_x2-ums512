// SPDX-License-Identifier: GPL-2.0-only
// EEBBK power-domain and clock owner for the pinned Mali-G52 register core.
#pragma once
#include <IOKit/IOTypes.h>
#include <stdint.h>
class NativeMaliPower final {
    volatile uint8_t *pmu_,*aon_,*clock_;
    uint32_t savedPmu_=0,aonAdded_=0,savedClock_[3]{};
    bool acquired_=false;
    static uint32_t read(volatile uint8_t *p,unsigned at){
        uint32_t value=*(volatile uint32_t *)(p+at);
        __asm__ volatile("dsb ld":::"memory");return value;
    }
    static void write(volatile uint8_t *p,unsigned at,uint32_t value){
        __asm__ volatile("dsb st":::"memory");*(volatile uint32_t *)(p+at)=value;
        __asm__ volatile("dsb sy":::"memory");
    }
public:
    NativeMaliPower(volatile uint8_t *p,volatile uint8_t *a,volatile uint8_t *c):pmu_(p),aon_(a),clock_(c){}
    bool prepared() const{return pmu_ && aon_ && clock_;}
    IOReturn acquire(bool factorySupplyQualified){
        if(!prepared() || !factorySupplyQualified)return kIOReturnNotReady;
        if(acquired_)return kIOReturnBusy;
        constexpr uint32_t domainOff=1u<<25,apbGate=1u<<11;
        savedPmu_=read(pmu_,0x30);
        write(pmu_,0x30,savedPmu_&~domainOff);
        aonAdded_=(~read(aon_,0))&apbGate;
        if(aonAdded_)write(aon_,0x1000,aonAdded_);
        for(unsigned i=0;i<3;i++)savedClock_[i]=read(clock_,4+4*i);
        uint32_t expectedClock[3];
        for(unsigned i=0;i<2;i++){
            expectedClock[i]=(savedClock_[i]&~0x771u)|1u;
            write(clock_,4+4*i,expectedClock[i]);
        }
        expectedClock[2]=(savedClock_[2]&~0x71u)|1u;
        write(clock_,0xc,expectedClock[2]);
        acquired_=true;
        if((read(pmu_,0x30)&domainOff) || !(read(aon_,0)&apbGate)){
            (void)release();return kIOReturnNotReady;
        }
        for(unsigned i=0;i<3;i++)if((read(clock_,4+4*i)&(i==2?0x71u:0x771u)) !=
            (expectedClock[i]&(i==2?0x71u:0x771u))){
            (void)release();return kIOReturnNotReady;
        }
        return kIOReturnSuccess;
    }
    IOReturn release(){
        if(!acquired_)return kIOReturnSuccess;
        IOReturn result=kIOReturnSuccess;
        for(unsigned i=0;i<3;i++){
            const uint32_t mask=i==2?0x71u:0x771u;
            write(clock_,4+4*i,(read(clock_,4+4*i)&~mask)|(savedClock_[i]&mask));
            if((read(clock_,4+4*i)&mask)!=(savedClock_[i]&mask))result=kIOReturnIOError;
        }
        if(aonAdded_){
            write(aon_,0x2000,aonAdded_);
            if(read(aon_,0)&(1u<<11))result=kIOReturnIOError;
        }
        aonAdded_=0;
        write(pmu_,0x30,(read(pmu_,0x30)&~(1u<<25))|(savedPmu_&(1u<<25)));
        if((read(pmu_,0x30)&(1u<<25))!=(savedPmu_&(1u<<25)))result=kIOReturnIOError;
        acquired_=false;
        return result;
    }
    bool ownsPower() const{return acquired_;}
    IOReturn gpuReadable() const{
        if(!prepared())return kIOReturnNotReady;
        if((read(pmu_,0x30)&(1u<<25)) || !(read(aon_,0)&(1u<<11)))return kIOReturnNotReady;
        for(unsigned i=0;i<3;i++)if(!(read(clock_,4+4*i)&1))return kIOReturnNotReady;
        return kIOReturnSuccess;
    }
};
