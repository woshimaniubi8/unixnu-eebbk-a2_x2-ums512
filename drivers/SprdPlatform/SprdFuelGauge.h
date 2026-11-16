// SPDX-License-Identifier: GPL-2.0-only
// SC2730 read protocol from the shipped 1h190 sc27xx_efuse_read and FGU
// calibration, cross-checked with Spreadtrum's GPL sc27xx-efuse.c.
#pragma once
#include "SprdBus.h"

namespace SprdBus {
struct FuelCalibration {
    uint16_t efuse=0;
    unsigned voltageAdcPerVolt=0,currentAdcPerAmp=0;
};
struct FuelReadTrace {
    unsigned phase=0;uint32_t gate=0,control=0,token=0;
    uint16_t status=0,module=0,mode=0;
};
struct FuelOcvPoint {uint32_t microvolts;uint8_t percent;};

inline bool fuelFirstPowerOn(uint16_t userArea) {
    const unsigned mode=(userArea>>12)&0xf;
    const unsigned capacity=userArea&0xfff;
    return mode==0xf || mode==0xa || capacity==0xfff;
}


inline FuelCalibration fuelCalibration(uint16_t efuse) {
    FuelCalibration c;
    c.efuse=efuse;
    c.voltageAdcPerVolt=(((unsigned)(efuse&0x1ff)+2611)*10+21)/42;
    // Factory DT: spec20000 uOhm, real10000 uOhm. This ratio differs
    // from the newer mainline FGU driver and must not be substituted.
    c.currentAdcPerAmp=(c.voltageAdcPerVolt*4*20000+5000)/10000;
    return c;
}

inline Result fuelMeasurements(unsigned voltage,unsigned current,const FuelCalibration &c,int &mv,int &ma) {
    // ADI returns the whole16-bit current register. The shipped driver does
    // not truncate it to14bits (high charging current crosses that boundary).
    if(voltage>8191 || current>65535 || c.voltageAdcPerVolt<622 || c.voltageAdcPerVolt>744 ||
       c.currentAdcPerAmp!=c.voltageAdcPerVolt*8)return Invalid;
    mv=(int)((voltage*1000+c.voltageAdcPerVolt/2)/c.voltageAdcPerVolt);
    int delta=(int)current-8192;
    ma=delta<0?-(int)((-delta*1000+c.currentAdcPerAmp/2)/c.currentAdcPerAmp):
               (int)((delta*1000+c.currentAdcPerAmp/2)/c.currentAdcPerAmp);
    return Ok;
}

inline Result fuelSavedDeciPercent(uint16_t userArea,int &deciPercent) {
    if(fuelFirstPowerOn(userArea))return Invalid;
    unsigned whole=userArea&0xff,part=(userArea>>8)&0xf;
    if(whole>100 || part>9 || (whole==100 && part))return Invalid;
    deciPercent=(int)(whole*10+part);return Ok;
}

inline Result fuelBootOcv(unsigned pocvAdc,unsigned qmaxAdc,const FuelCalibration &c,
                          unsigned internalResistanceMicroOhms,int &ocvMicrovolts,int &openCurrentMilliamps) {
    if(qmaxAdc>32767 || internalResistanceMicroOhms>1000000)return Invalid;
    const unsigned firstCurrentAdc=qmaxAdc*2;
    int voltageMillivolts=0;
    Result r=fuelMeasurements(pocvAdc,firstCurrentAdc,c,voltageMillivolts,openCurrentMilliamps);
    if(r!=Ok)return r;
    // The shipped get_boot_voltage treats POCV<=3449mV as unqualified and
    // switches to a live-OCV path. This read-only stage does not substitute
    // a different sample; retain the failed qualification in the log.
    if(voltageMillivolts<=3449 || voltageMillivolts>5000)return Invalid;
    const int64_t ocv=(int64_t)voltageMillivolts*1000-
                      (int64_t)openCurrentMilliamps*(internalResistanceMicroOhms/1000);
    if(ocv<=0 || ocv>6000000)return Invalid;
    ocvMicrovolts=(int)ocv;
    return Ok;
}

inline Result fuelOcvCapacity(const FuelOcvPoint *table,unsigned count,unsigned ocvMicrovolts,int &percent) {
    if(!table || count<2 || count>64)return Invalid;
    for(unsigned i=0;i<count;i++) {
        if(table[i].percent>100 || (i && (table[i-1].microvolts<=table[i].microvolts ||
                                           table[i-1].percent<table[i].percent)))return Invalid;
    }
    if(ocvMicrovolts>table[0].microvolts){percent=table[0].percent;return Ok;}
    if(ocvMicrovolts<=table[count-1].microvolts){percent=table[count-1].percent;return Ok;}
    for(unsigned i=1;i<count;i++) {
        if(table[i].microvolts<ocvMicrovolts) {
            const uint32_t voltageSpan=table[i-1].microvolts-table[i].microvolts;
            const unsigned percentSpan=table[i-1].percent-table[i].percent;
            percent=table[i].percent+(unsigned)((uint64_t)(ocvMicrovolts-table[i].microvolts)*percentSpan/voltageSpan);
            return Ok;
        }
    }
    return Invalid;
}

// The shipped capacity routine reads +0x6c/+0x68. Qualify the split counter
// across a rollover instead of combining halves from different samples.
template<class AdiBus> Result fuelCounterRead(AdiBus &adi,uint32_t &counter) {
    for(unsigned attempt=0;attempt<4;attempt++) {
        uint16_t high=0,low=0,check=0;
        Result r=adi.read(0xc68,high);if(r!=Ok)return r;
        r=adi.read(0xc6c,low);if(r!=Ok)return r;
        r=adi.read(0xc68,check);if(r!=Ok)return r;
        if(high==check){counter=((uint32_t)high<<16)|low;return Ok;}
    }
    return Protocol;
}

inline int64_t fuelRoundSigned(int64_t value,unsigned divisor) {
    return value<0?-((-value+divisor/2)/divisor):(value+divisor/2)/divisor;
}

inline Result fuelCounterDeltaMah(int64_t counts,const FuelCalibration &c,int64_t &mah) {
    if(c.voltageAdcPerVolt<622 || c.voltageAdcPerVolt>744 ||
       c.currentAdcPerAmp!=c.voltageAdcPerVolt*8 ||
       counts<-(1LL<<46) || counts>(1LL<<46))return Invalid;
    // Original 1h190 get_capacity @ffffff80087b4b00: 2Hz, 3.6C/mAh;
    // retain BOTH signed roundings before the percentage conversion.
    mah=fuelRoundSigned(fuelRoundSigned(counts*10,72),c.currentAdcPerAmp);
    return Ok;
}

struct FuelCounterState {
    bool initialized=false;
    uint32_t last=0;
    uint64_t lastNanoseconds=0;
    int initialDeciPercent=0;
    int64_t accumulated=0;
    Result begin(int deciPercent,uint32_t counter,uint64_t now) {
        if(deciPercent<0 || deciPercent>1000)return Invalid;
        initialDeciPercent=deciPercent;last=counter;lastNanoseconds=now;
        accumulated=0;initialized=true;return Ok;
    }
    Result observe(uint32_t counter,uint64_t now,const FuelCalibration &c,int &deciPercent) {
        if(!initialized || now<=lastNanoseconds)return Invalid;
        const uint64_t elapsed=now-lastNanoseconds;
        // Two samples/second, bounded by the complete 16-bit ADC range,
        // plus two sample periods. A reset/jump is an error, not a new base.
        if(elapsed>3600000000000ULL)return Protocol;
        const uint64_t bound=elapsed*131072/1000000000+131072;
        const uint32_t wrapped=counter-last;
        const int64_t delta=wrapped&0x80000000u?(int64_t)wrapped-0x100000000LL:wrapped;
        if(delta>(int64_t)bound || delta<-(int64_t)bound)return Protocol;
        const int64_t total=accumulated+delta;int64_t mah=0;
        Result r=fuelCounterDeltaMah(total,c,mah);if(r!=Ok)return r;
        int64_t capacity=initialDeciPercent+fuelRoundSigned(mah*1000,4900);
        deciPercent=(int)(capacity<0?0:capacity>1000?1000:capacity);
        last=counter;lastNanoseconds=now;accumulated=total;return Ok;
    }
};

struct ChargerState {
    bool external=false,chargeCapable=false,charging=false,terminated=false;
    unsigned source=0,phase=0;
};
inline ChargerState chargerState(uint8_t status,uint8_t liveFault,uint8_t input,uint8_t control) {
    ChargerState s;s.source=status>>5;s.phase=(status>>3)&3;
    s.external=(status&4) && s.source>0 && s.source<7 && !(control&0x20);
    // Watchdog-expired (bit7) is reported separately. Charge/NTC/battery
    // faults and input HIZ prevent a charge-capable claim.
    const unsigned ntc=liveFault&7;
    const bool temperatureAllowsCharge=ntc==0 || ntc==2 || ntc==3; // normal/warm/cool
    s.chargeCapable=s.external && !(input&0x80) && !(liveFault&0x38) && temperatureAllowsCharge;
    s.charging=s.chargeCapable && (control&0x10) && (s.phase==1 || s.phase==2);
    s.terminated=s.chargeCapable && s.phase==3;return s;
}

template<class AdiBus,class HwLock,class Aon> class FuelGaugeCalibration {
    AdiBus &adi;HwLock &hw;Aon &aon;
    FuelReadTrace trace;
    Result wait(uint16_t mask) {
        for(unsigned n=0;n<3000;n++) {
            uint16_t status=0;Result r=adi.read(0x314,status);
            trace.status=status;
            if(r!=Ok)return r;
            if(status&mask)return Ok;
            hw.delay(1000);
        }
        return Timeout;
    }
public:
    FuelGaugeCalibration(AdiBus &a,HwLock &h,Aon &c):adi(a),hw(h),aon(c) {}
    const FuelReadTrace &diagnostic() const {return trace;}
    Result read(FuelCalibration &out) {
        trace={};trace.phase=1;
        // Gate index25 in the actual onecell table, including its leading
        // count word/padding: splk-eb, AON+4 bit10. Index24 is USB PHY.
        const uint32_t added=(~aon.read32(4))&(1u<<10);
        if(added)aon.write32(0x1004,added);
        trace.gate=aon.read32(4);
        if(!(trace.gate&(1u<<10))){if(added)aon.write32(0x2004,added);return Protocol;}
        uint32_t control=hw.read32(4);bool changed=(control&1)==0;
        if(changed)hw.write32(4,control|1);
        trace.control=hw.read32(4);trace.phase=2;
        Result result=Timeout;bool acquired=false,moduleSaved=false,indexSaved=false,started=false;
        uint16_t module=0,index=0,mode=0,value=0;
        // RECCTRL(+4) records the lock's subsystem owner; it does not enable
        // TOKEN reads and stock never gates trylock on its readback value.
        trace.phase=3;
        for(unsigned n=0;n<5000;n++) {
            trace.token=hw.read32(0x830);
            if(trace.token==0){acquired=true;break;}
            hw.delay(10);
        }
        if(acquired) {
            trace.phase=4;
            result=adi.read(0x1808,module);moduleSaved=result==Ok;
            trace.module=module;
            if(result==Ok)result=adi.update(0x1808,0x40,0x40);
            if(result==Ok){trace.phase=5;result=wait(4);}
            if(result==Ok){trace.phase=6;result=adi.read(0x30c,index);indexSaved=result==Ok;}
            if(result==Ok){trace.phase=7;result=adi.read(0x310,mode);trace.mode=mode;}
            // Never preserve/reissue a programming-start command. Only the
            // read engine is used; DATA_WR and timing registers are untouched.
            if(result==Ok && (mode&1))result=Protocol;
            if(result==Ok)result=adi.write(0x30c,3); // DT nvmem byteoffset6.
            if(result==Ok){trace.phase=8;result=adi.write(0x310,(uint16_t)((mode&~7u)|2));started=result==Ok;}
            if(result==Ok){trace.phase=9;result=wait(0x10);}
            if(result==Ok){trace.phase=10;result=adi.read(0x304,value);}
            if(started) {
                Result r=adi.write(0x310,(uint16_t)((mode&~7u)|4));
                if(result==Ok)result=r;
            }
            if(indexSaved) {Result r=adi.write(0x30c,index);if(result==Ok)result=r;}
            if(moduleSaved) {Result r=adi.update(0x1808,0x40,module);if(result==Ok)result=r;}
            hw.write32(0x830,0x55aa10c5);
        }
        if(changed)hw.write32(4,control);
        if(added)aon.write32(0x2004,added);
        if(result==Ok)out=fuelCalibration(value);
        return result;
    }
};
}
