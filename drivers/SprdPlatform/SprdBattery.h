// SPDX-License-Identifier: GPL-2.0-only
// Standard IOPMPowerSource frontend adapted from UniXNU-sheng ShengBattery.cpp
// (BSD-2-Clause-Patent; see LICENSE-UniXNU-frontend.txt). Hardware sampling and
// capacity acquisition remain on NativeSprdPlatform's serialized work loop.
#pragma once
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSNumber.h>
#include "SprdFuelGauge.h"

class NativeSprdBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(NativeSprdBattery)
    bool published=false;
    unsigned samples=0;
    void number(const char *key,int64_t value) {
        const OSSymbol *symbol=OSSymbol::withCString(key);
        OSNumber *n=OSNumber::withNumber((unsigned long long)value,32);
        if(symbol && n)setPSProperty(symbol,n);
        OSSafeReleaseNULL(n);OSSafeReleaseNULL(symbol);
    }
    void flag(const char *key,bool value) {
        const OSSymbol *symbol=OSSymbol::withCString(key);
        if(symbol)setPSProperty(symbol,value?kOSBooleanTrue:kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }
public:
    void acquired(int deciPercent,int millivolts,int milliamps,bool present,
                  const SprdBus::ChargerState &charger,uint8_t latchedFault,uint8_t liveFault,
                  uint32_t counter,int64_t deltaCounts,const char *origin) {
        const unsigned percent=(unsigned)(deciPercent+5)/10;
        setBatteryInstalled(present);
        setCurrentCapacity(percent);setMaxCapacity(100);
        setVoltage((unsigned)millivolts);setAmperage(milliamps);
        setExternalConnected(charger.external);
        setExternalChargeCapable(charger.chargeCapable);
        setIsCharging(present && charger.charging);
        setAtWarnLevel(present && percent<=10);
        setAtCriticalLevel(present && percent<=5);
        setTimeRemaining(0xffff); // no qualified learned time-to-empty/full
        flag("FullyCharged",present && charger.terminated && percent>=99);
        number("InstantAmperage",milliamps);number("DesignCapacity",4900);
        number("FGUCoulombCounter",counter);number("ChargerFaultLatched",latchedFault);
        number("ChargerFaultCurrent",liveFault);number("ChargerState",charger.phase);
        flag("HardwareChargeTerminated",charger.terminated);
        setProperty("CapacityOrigin",origin);
        setProperty("HardwareSampleValid",true);
        setProperty("LearnedFullCapacityQualified",false);
        setProperty("TemperatureCompensationQualified",false);
        if(!published){published=true;registerService();}
        updateStatus();
        if(samples<3 || !(samples%12))IOLog("M114 battery: sample=%u published=%u capacity=%u/100 deci=%d mv=%d ma=%d present=%u external=%u capable=%u charging=%u terminated=%u faults=%02x/%02x counter=0x%x delta=%lld source=%s\n",
            samples,published,percent,deciPercent,millivolts,milliamps,present,charger.external,
            charger.chargeCapable,charger.charging,charger.terminated,latchedFault,liveFault,
            counter,deltaCounts,origin);
        samples++;
    }
    void unavailable(SprdBus::Result reason) {
        setProperty("HardwareSampleValid",false);
        setProperty("HardwareSampleError",(unsigned)reason,32);
    }
    void removed() {
        setBatteryInstalled(false);setIsCharging(false);
        unavailable(SprdBus::Invalid);updateStatus();
    }
};
OSDefineMetaClassAndStructors(NativeSprdBattery,IOPMPowerSource)
