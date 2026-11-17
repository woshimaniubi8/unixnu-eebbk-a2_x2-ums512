// SPDX-License-Identifier: GPL-2.0-only
// Spreadtrum PWM register contract from pwm-sprd.c, verified against the
// shipped 4.14.193 sprd_pwm_{config,get_state,enable,disable} instructions.
#pragma once
#include <stdint.h>
#include "SprdBus.h"

namespace SprdBus {
struct PwmState {uint32_t prescale=0,mod=0,duty=0,enable=0,division=0,patternLow=0,patternHigh=0;};
struct PwmWriteTrace {unsigned phase=0;PwmState before,after;};
template<class IO> class Pwm {
    IO &io;unsigned base;PwmWriteTrace trace;bool factoryConfigured=false;
    void commit(uint32_t prescale,uint32_t mod,uint32_t duty,uint32_t low,uint32_t high) {
        // Exact shipped4.14 sprd_pwm_config store order. PRESCALE is the
        // final commit, unlike the newer mainline DUTY-last implementation.
        io.write32(base+4,mod);io.write32(base+8,duty);
        io.write32(base+0x10,low);io.write32(base+0x14,high);
        io.write32(base,prescale);
    }
public:
    explicit Pwm(IO &i,unsigned channel):io(i),base(channel*0x20) {}
    const PwmWriteTrace &diagnostic() const {return trace;}
    Result read(PwmState &s) {
        if(base>=4*0x20)return Invalid;
        s={io.read32(base),io.read32(base+4),io.read32(base+8),io.read32(base+0x18),
           io.read32(base+0xc),io.read32(base+0x10),io.read32(base+0x14)};
        // Take over the configured waveform. Never invent a PWM period or
        // source clock when boot firmware has left no usable configuration.
        // The shipped 4.14 get_state masks PRESCALE with0xffff. Firmware
        // leaves0x8003 here; the newer mainline8-bit mask is not this ABI.
        // M110 observed bit15 changing on disable. Its precise hardware role
        // remains unqualified: that observation does not prove the inherited
        // waveform is flicker-free. Configure the original75us recipe first.
        return s.prescale<=65535 && s.mod==255 && s.duty<=255 && !(s.enable&~1u)?Ok:Protocol;
    }
    Result configureFactory(uint32_t sourceHz,uint32_t periodNs) {
        trace={};trace.phase=1;
        if(sourceHz!=26000000 || periodNs!=75000)return Invalid;
        Result r=read(trace.before);if(r!=Ok)return r;
        const PwmState &before=trace.before;
        // Original config with ext26m and75000ns computes PRESCALE6, MOD255,
        // continuous patternffff/ffff. Do not inherit cboot8003 or its pattern.
        commit(6,255,before.duty,0xffff,0xffff);
        trace.phase=2;r=read(trace.after);if(r!=Ok)return r;
        trace.phase=3;const PwmState &after=trace.after;
        if((after.prescale&0x7fff)!=6 || after.mod!=255 || after.duty!=before.duty ||
           after.enable!=before.enable || after.division!=before.division ||
           after.patternLow!=0xffff || after.patternHigh!=0xffff)return Protocol;
        factoryConfigured=true;trace.phase=4;return Ok;
    }
    Result duty(uint32_t value,bool enabled) {
        trace={};trace.phase=1;
        if(value>255)return Invalid;
        Result r=read(trace.before);if(r!=Ok)return r;
        const PwmState &before=trace.before;
        if(!factoryConfigured || (before.prescale&0x7fff)!=6 ||
           before.patternLow!=0xffff || before.patternHigh!=0xffff)return Protocol;
        // Do not rewrite ENABLE on every fade step. Stage the factory tuple
        // and commit PRESCALE last; enable only on an actual off/on transition.
        if(!enabled && before.enable)io.write32(base+0x18,0);
        if(before.duty!=value)commit(6,255,value,0xffff,0xffff);
        if(enabled && !before.enable)io.write32(base+0x18,1);
        trace.phase=2;r=read(trace.after);if(r!=Ok)return r;
        trace.phase=3;const PwmState &after=trace.after;
        if((after.prescale&0x7fffu)!=6 || after.mod!=before.mod ||
           after.duty!=value || after.enable!=(enabled?1u:0u) ||
           after.division!=before.division || after.patternLow!=0xffff || after.patternHigh!=0xffff)return Protocol;
        trace.phase=4;return Ok;
    }
    Result restore(const PwmState &s) {
        if(base>=4*0x20 || s.prescale>65535 || s.mod!=255 || s.duty>255 || s.enable>1)return Invalid;
        io.write32(base+0x18,0);
        commit(s.prescale,s.mod,s.duty,s.patternLow,s.patternHigh);io.write32(base+0x18,s.enable);
        factoryConfigured=false;
        PwmState actual;Result r=read(actual);
        return r==Ok && !((actual.prescale^s.prescale)&0x7fffu) && actual.mod==s.mod &&
            actual.duty==s.duty && actual.enable==s.enable && actual.division==s.division &&
            actual.patternLow==s.patternLow && actual.patternHigh==s.patternHigh?Ok:Protocol;
    }
};
}
