// SPDX-License-Identifier: GPL-2.0-only
// Spreadtrum PWM register contract from pwm-sprd.c, verified against the
// shipped 4.14.193 sprd_pwm_{config,get_state,enable,disable} instructions.
#pragma once
#include <stdint.h>
#include "SprdBus.h"

namespace SprdBus {
struct PwmState {uint32_t prescale=0,mod=0,duty=0,enable=0;};
struct PwmWriteTrace {unsigned phase=0;PwmState before,after;};
template<class IO> class Pwm {
    IO &io;unsigned base;PwmWriteTrace trace;
public:
    explicit Pwm(IO &i,unsigned channel):io(i),base(channel*0x20) {}
    const PwmWriteTrace &diagnostic() const {return trace;}
    Result read(PwmState &s) {
        if(base>=4*0x20)return Invalid;
        s={io.read32(base),io.read32(base+4),io.read32(base+8),io.read32(base+0x18)};
        // Take over the configured waveform. Never invent a PWM period or
        // source clock when boot firmware has left no usable configuration.
        // The shipped 4.14 get_state masks PRESCALE with0xffff. Firmware
        // leaves0x8003 here; the newer mainline8-bit mask is not this ABI.
        // Preserve the whole configured divider, including its upper bits.
        return s.prescale<=65535 && s.mod==255 && s.duty<=255 && !(s.enable&~1u)?Ok:Protocol;
    }
    Result duty(uint32_t value,bool enabled) {
        trace={};trace.phase=1;
        if(value>255)return Invalid;
        Result r=read(trace.before);if(r!=Ok)return r;
        const PwmState &before=trace.before;
        // DUTY commits the running waveform. Preserve period, clock, pattern
        // and other PWM channels; do not reset the block on a brightness set.
        if(!enabled)io.write32(base+0x18,0);
        io.write32(base+8,value);
        if(enabled)io.write32(base+0x18,1);
        trace.phase=2;r=read(trace.after);if(r!=Ok)return r;
        trace.phase=3;const PwmState &after=trace.after;
        if(after.prescale!=before.prescale || after.mod!=before.mod ||
           after.duty!=value || after.enable!=(enabled?1u:0u))return Protocol;
        trace.phase=4;return Ok;
    }
    Result restore(const PwmState &s) {
        if(base>=4*0x20 || s.prescale>65535 || s.mod!=255 || s.duty>255 || s.enable>1)return Invalid;
        io.write32(base+0x18,0);
        io.write32(base,s.prescale);io.write32(base+4,s.mod);
        io.write32(base+8,s.duty);io.write32(base+0x18,s.enable);
        PwmState actual;Result r=read(actual);
        return r==Ok && actual.prescale==s.prescale && actual.mod==s.mod &&
            actual.duty==s.duty && actual.enable==s.enable?Ok:Protocol;
    }
};
}
