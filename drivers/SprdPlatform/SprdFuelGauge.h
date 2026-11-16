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
    if(voltage>8191 || current>16383 || c.voltageAdcPerVolt<622 || c.voltageAdcPerVolt>744 ||
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
    if(qmaxAdc>8191 || internalResistanceMicroOhms>1000000)return Invalid;
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
