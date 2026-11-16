// SPDX-License-Identifier: GPL-2.0-only
#include "SprdFuelGauge.h"
#include <cassert>
#include <map>
#include <cstdio>
#include <vector>
using namespace SprdBus;
struct AdiModel {
    std::map<unsigned,uint16_t> regs{{0x1808,0x8291},{0x30c,19},{0x310,0x80},{0x304,125}};
    unsigned calls=0,fail=0,commands=0;bool standby=true,done=true;
    Result read(unsigned off,uint16_t &v) {
        if(++calls==fail)return Protocol;
        if(off==0x314)v=(standby?4:0)|((commands && done)?0x10:0);
        else {assert(regs.count(off));v=regs[off];}
        return Ok;
    }
    Result write(unsigned off,uint16_t value) {
        if(++calls==fail)return Protocol;
        assert(off==0x1808 || off==0x30c || off==0x310);
        if(off==0x310){assert(!(value&1));assert((value&7)==2 || (value&7)==4);if(value&2)commands++;}
        regs[off]=value;return Ok;
    }
    Result update(unsigned off,uint16_t mask,uint16_t value) {
        uint16_t b=0;Result r=read(off,b);return r==Ok?write(off,(b&~mask)|(value&mask)):r;
    }
};
struct Hardware {
    uint32_t control=0x80;bool busy=false,held=false,zeroRecordRead=false;unsigned unlocks=0,reads=0;uint64_t delayed=0;
    uint32_t read32(unsigned off) {
        if(off==4)return zeroRecordRead?0:control;
        assert(off==0x830);reads++;if(busy)return 1;
        assert(!held);held=true;return 0;
    }
    void write32(unsigned off,uint32_t v) {
        if(off==4){control=v;return;}
        assert(off==0x830 && v==0x55aa10c5 && held);held=false;unlocks++;
    }
    void delay(unsigned us){delayed+=us;}
};
struct Gate {
    uint32_t value=0x123000;bool refuse=false;
    uint32_t read32(unsigned off){assert(off==4);return value;}
    void write32(unsigned off,uint32_t v){assert(v==(1u<<10));assert(off==0x1004 || off==0x2004);if(!refuse)value=off==0x1004?value|v:value&~v;}
};
struct CounterModel {
    std::vector<uint16_t> values;
    unsigned at=0,fail=100;
    Result read(unsigned off,uint16_t &value) {
        assert(off==(at%3==1?0xc6c:0xc68));
        if(at==fail)return Timeout;
        assert(at<values.size());value=values[at++];return Ok;
    }
};
int main() {
    const FuelOcvPoint ocv[]={{4386000,100},{4329000,95},{4274000,90},{4217000,85},{4160000,80},{4106000,75},{4055000,70},{4008000,65},{3964000,60},{3905000,55},{3866000,50},{3840000,45},{3818000,40},{3800000,35},{3784000,30},{3766000,25},{3744000,20},{3719000,15},{3690000,10},{3679000,5},{3460000,0}};
    assert(fuelFirstPowerOn(0xafff) && fuelFirstPowerOn(0x5fff) && fuelFirstPowerOn(0xf064) && !fuelFirstPowerOn(0x5064));
    int percent=-1,ocvUv=0,openMa=0,savedDeci=-1;
    assert(fuelSavedDeciPercent(0x5352,savedDeci)==Ok && savedDeci==823);
    assert(fuelSavedDeciPercent(0x5064,savedDeci)==Ok && savedDeci==1000);
    assert(fuelSavedDeciPercent(0xafff,savedDeci)==Invalid);
    assert(fuelSavedDeciPercent(0x5a50,savedDeci)==Invalid);
    assert(fuelSavedDeciPercent(0x5100|100,savedDeci)==Invalid);
    assert(fuelOcvCapacity(ocv,21,3460000,percent)==Ok && percent==0);
    assert(fuelOcvCapacity(ocv,21,4160000,percent)==Ok && percent==80);
    assert(fuelOcvCapacity(ocv,21,4386000,percent)==Ok && percent==100);
    assert(fuelOcvCapacity(ocv,21,5000000,percent)==Ok && percent==100);
    assert(fuelOcvCapacity(ocv,21,3000000,percent)==Ok && percent==0);
    assert(fuelOcvCapacity(ocv,21,4133000,percent)==Ok && percent==77);
    assert(fuelOcvCapacity(ocv,1,4160000,percent)==Invalid);
    for(unsigned e=0;e<65536;e++) {
        auto c=fuelCalibration((uint16_t)e);int mv=-1,ma=-1;
        assert(c.voltageAdcPerVolt==(((e&511)+2611)*10+21)/42);
        assert(fuelMeasurements(c.voltageAdcPerVolt*4,8192,c,mv,ma)==Ok && mv==4000 && ma==0);
        assert(fuelMeasurements(2939,8192+c.currentAdcPerAmp,c,mv,ma)==Ok && ma==1000);
        assert(fuelMeasurements(2939,8192-c.currentAdcPerAmp,c,mv,ma)==Ok && ma==-1000);
    }
    int mv=123,ma=456;assert(fuelMeasurements(2939,8192,FuelCalibration{},mv,ma)==Invalid && mv==123 && ma==456);
    for(unsigned fail=0;fail<20;fail++) {
        AdiModel adi;Hardware hw;Gate gate;const auto saved=gate.value;adi.fail=fail;
        FuelGaugeCalibration reader(adi,hw,gate);FuelCalibration c;
        Result r=reader.read(c);
        assert(!hw.held && hw.unlocks==1 && hw.control==0x80 && gate.value==saved);
        if(r==Ok)assert(c.efuse==125 && adi.regs[0x1808]==0x8291 && adi.regs[0x30c]==19);
        else assert(c.voltageAdcPerVolt==0);
    }
    for(unsigned failure=0;failure<4;failure++) {
        AdiModel adi;Hardware hw;Gate gate;auto saved=gate.value;
        if(failure==0)hw.busy=true;
        if(failure==1)adi.standby=false;
        if(failure==2)adi.done=false;
        if(failure==3)adi.regs[0x310]|=1;
        FuelGaugeCalibration reader(adi,hw,gate);FuelCalibration c;
        assert(reader.read(c)!=Ok && !c.voltageAdcPerVolt && !hw.held && gate.value==saved);
        assert(hw.unlocks==(failure==0?0u:1u));
    }
    AdiModel adi;Hardware hw;Gate gate;gate.refuse=true;FuelGaugeCalibration reader(adi,hw,gate);FuelCalibration c;
    assert(reader.read(c)==Protocol && hw.reads==0 && adi.calls==0);
    AdiModel readable;Hardware recordOnly;Gate powered;recordOnly.control=0;recordOnly.zeroRecordRead=true;
    FuelGaugeCalibration actual(readable,recordOnly,powered);FuelCalibration qualified;
    assert(actual.read(qualified)==Ok && qualified.efuse==125 && recordOnly.unlocks==1 && !recordOnly.held);
    auto cal=fuelCalibration(0x775d);
    assert(fuelBootOcv(2939,4096,cal,134000,ocvUv,openMa)==Ok && ocvUv>4100000 && ocvUv<4200000);
    assert(fuelBootOcv(2939,0xffff,cal,134000,ocvUv,openMa)==Invalid);
    assert(fuelBootOcv(1000,4096,cal,134000,ocvUv,openMa)==Invalid);
    CounterModel coherent{{0x1234,0x5678,0x1234}};uint32_t counter=0;
    assert(fuelCounterRead(coherent,counter)==Ok && counter==0x12345678);
    CounterModel rollover{{0x1234,0xffff,0x1235,0x1235,7,0x1235}};
    assert(fuelCounterRead(rollover,counter)==Ok && counter==0x12350007);
    CounterModel broken{{1,2,3,4,5,6,7,8,9,10,11,12}};
    counter=123;assert(fuelCounterRead(broken,counter)==Protocol && counter==123);
    CounterModel unreadable{{1,2,1}};unreadable.fail=1;
    assert(fuelCounterRead(unreadable,counter)==Timeout && counter==123);
    int64_t mah=0;const int countsPerMah=cal.currentAdcPerAmp*72/10;
    assert(fuelCounterDeltaMah(countsPerMah,cal,mah)==Ok && mah==1);
    assert(fuelCounterDeltaMah(-countsPerMah,cal,mah)==Ok && mah==-1);
    FuelCounterState state;int deci=-1;
    assert(state.observe(1,1,cal,deci)==Invalid);
    assert(state.begin(1001,0,0)==Invalid);
    assert(state.begin(800,0xfffffff0,1)==Ok);
    assert(state.observe(0x20,1000000001,cal,deci)==Ok && deci==800 && state.accumulated==48);
    auto saved=state;
    assert(state.observe(0x70000000,2000000001,cal,deci)==Protocol);
    assert(state.last==saved.last && state.accumulated==saved.accumulated && state.lastNanoseconds==saved.lastNanoseconds);
    assert(state.observe(0x20,1000000001,cal,deci)==Invalid);
    assert(state.begin(800,12345,1)==Ok);
    for(unsigned i=1;i<=72;i++) {
        // 1A over an hour adds 1000mAh against the exact factory 4900mAh.
        assert(state.observe((uint32_t)(12345+(uint64_t)countsPerMah*1000*i/72),i*50000000000ULL+1,cal,deci)==Ok);
    }
    assert(deci==1000);
    assert(state.begin(800,0x20000000,1)==Ok);
    for(unsigned i=1;i<=72;i++)assert(state.observe((uint32_t)(0x20000000-(uint64_t)countsPerMah*1000*i/72),i*50000000000ULL+1,cal,deci)==Ok);
    assert(deci==596);
    auto charger=chargerState(0xbc,0,0,0x10);
    assert(charger.external && charger.chargeCapable && !charger.charging && charger.terminated);
    charger=chargerState(0xb4,0,0,0x10);assert(charger.charging && !charger.terminated);
    charger=chargerState(0xb4,0x80,0,0x10);assert(charger.charging);
    charger=chargerState(0xb4,0x10,0,0x10);assert(!charger.charging && !charger.chargeCapable);
    charger=chargerState(0xb4,2,0,0x10);assert(charger.charging);
    charger=chargerState(0xb4,5,0,0x10);assert(!charger.charging);
    charger=chargerState(0xb4,0,0x80,0x10);assert(!charger.charging);
    charger=chargerState(0xf4,0,0,0x30);assert(!charger.external && !charger.charging);
    std::puts("PASS: coherent read-only CLBCNT acquisition, rollovers, signed factory counter units, 1A-hour charge/discharge, reset rejection and real charger state");
    std::puts("PASS: SC2730 read-only efuse, factory first-poweron flag, exact OCV table interpolation, QMAX/POCV math, fault cleanup and no FGU programming commands");
}
