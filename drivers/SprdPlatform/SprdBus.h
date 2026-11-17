// SPDX-License-Identifier: GPL-2.0-only
// Register protocols adapted from Linux Spreadtrum spi-sprd-adi.c, gpio-sprd.c,
// gpio-pmic-eic-sprd.c and i2c-sprd.c (Spreadtrum 2017), checked against the
// shipped EEBBK 4.14.193 driver. See README.md for the pinned source manifest.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace SprdBus {
enum Result { Ok=0, Invalid=-1, Timeout=-2, Protocol=-3, Nack=-4, Busy=-5 };
constexpr uint32_t kAdiSlave = 0x20000;
constexpr uint32_t kAdiPhysical = 0x32120000;
template<class IO> class Adi {
    IO &io;
    bool wait(uint32_t mask, bool set, unsigned limit) {
        while (limit--) {
            if (!!(io.read32(0x30)&mask)==set) return true;
            io.delay(1);
        }
        return false;
    }
public:
    explicit Adi(IO &i):io(i) {}
    Result read(uint32_t reg,uint16_t &value) {
        if ((reg&3) || reg>=0x20000) return Invalid;
        const uint32_t address=kAdiPhysical+reg;
        io.write32(0x28,address);
        for (unsigned i=0;i<2000;i++) {
            uint32_t r=io.read32(0x2c);
            if (!(r&0x80000000)) {
                if (((r>>16)^(address>>2))&0x7fff) return Protocol;
                value=(uint16_t)r;
                return Ok;
            }
            io.delay(1);
        }
        return Timeout;
    }
    Result write(uint32_t reg,uint16_t value) {
        if ((reg&3) || reg>=0x20000) return Invalid;
        if (!wait(1u<<10,true,1000) || !wait(1u<<11,false,1000)) return Timeout;
        io.write32(kAdiSlave+reg,value);
        return wait(1u<<31,false,2000) ? Ok : Timeout;
    }
    Result update(uint32_t reg,uint16_t mask,uint16_t value) {
        uint16_t before=0; Result r=read(reg,before);
        if(r!=Ok)return r;
        const uint16_t after=(uint16_t)((before&~mask)|(value&mask));
        return after==before ? Ok : write(reg,after);
    }
};

// Short register messages fit the hardware's 12-byte FIFO. Larger device
// reads are split at the device-register layer, never truncated in the bus.
// Poll the same completion bit serviced by Linux's IRQ handler, with a bounded
// deadline; do not substitute successful transfers when NACK/timeout occurs.
template<class IO> class I2c {
    IO &io;
public:
    explicit I2c(IO &i):io(i) {}
    void configure26000kHz400kHz() {
        const uint32_t prescale=26000000/(4*400000)-1;
        const uint32_t high=prescale*4/5,low=prescale*6/5;
        io.write32(0,0x100|(15u<<16)|(4u<<12));
        io.write32(0x20,(high<<16)|low);
        io.write32(0x24,0); io.write32(0x28,15);
        io.write32(0x2c,1);
        io.write32(0x14,io.read32(0x14)&~6u);
        // Stock sprd_i2c_enable() sets both EN and INT_EN. INT_EN gates the
        // completion indication polled below, even with the GIC line masked.
        // Enabling the peripheral latch does not install a CPU IRQ handler.
        io.write32(0,io.read32(0)|6u);
    }
    Result message(uint8_t address,uint8_t *data,size_t size,bool read,bool stop) {
        if(address<8 || address>0x77 || !data || !size || size>12) return Invalid;
        io.write32(0x2c,1);
        io.write32(0x14,io.read32(0x14)&~6u);
        io.write32(4,(uint32_t)address<<1); io.write32(8,(uint32_t)size);
        uint32_t ctl=io.read32(0)&~(1u|8u|(1u<<9)|(1u<<10)|(1u<<11)|(1u<<20));
        if(read)ctl|=8;
        if(!stop)ctl|=1u<<20;
        io.write32(0,ctl);
        if(!read) for(size_t i=0;i<size;i++)io.write8(0x10,data[i]);
        io.write32(0,ctl|1);
        for(unsigned i=0;i<5000;i++) {
            const uint32_t status=io.read32(0x14);
            if(status&4) {
                Result result=(status&2) ? Nack : Ok;
                if(result==Ok && read)for(size_t j=0;j<size;j++)data[j]=io.read8(0xc);
                io.write32(0,io.read32(0)&~1u);
                io.write32(0x14,io.read32(0x14)&~6u);
                return result;
            }
            io.delay(10);
        }
        io.write32(0,io.read32(0)&~1u);
        io.write32(0x2c,1);
        return Timeout;
    }
    Result readRegister(uint8_t address,uint16_t reg,unsigned width,uint8_t *data,size_t size) {
        if(!data || !size || (width!=1 && width!=2) || (width==1 && (reg>255 || size>256u-reg)) || size>0x10000u-reg)return Invalid;
        while(size) {
            const size_t n=size>8 ? 8 : size;
            if(width==1 && n>256u-reg)return Invalid;
            uint8_t pointer[2]={(uint8_t)(width==2 ? reg>>8 : reg),(uint8_t)reg};
            Result r=message(address,pointer,width,false,false);
            if(r!=Ok)return r;
            r=message(address,data,n,true,true);
            if(r!=Ok)return r;
            reg=(uint16_t)(reg+n); data+=n; size-=n;
        }
        return Ok;
    }
    Result writeRegister(uint8_t address,uint16_t reg,unsigned width,const uint8_t *data,size_t size) {
        if(!data || !size || (width!=1 && width!=2) || size>12-width || (width==1 && reg>255))return Invalid;
        uint8_t bytes[12]; bytes[0]=(uint8_t)(width==2 ? reg>>8 : reg);
        if(width==2)bytes[1]=(uint8_t)reg;
        for(size_t i=0;i<size;i++)bytes[width+i]=data[i];
        return message(address,bytes,width+size,false,true);
    }
};

struct Contact { uint8_t id; uint16_t x,y; };
struct Frame { unsigned count; Contact contacts[10]; };
inline Result goodixFrame(const uint8_t *p,size_t bytes,unsigned count,unsigned width,unsigned height,Frame &out) {
    if(!p || count>10 || bytes<count*8 || width<2 || height<2)return Invalid;
    Frame frame={}; uint16_t seen=0;
    for(unsigned i=0;i<count;i++) {
        const uint8_t *c=p+i*8; unsigned id=c[0]&15;
        unsigned x=(unsigned)c[1]|((unsigned)c[2]<<8),y=(unsigned)c[3]|((unsigned)c[4]<<8);
        if(c[0]&0x80 || (seen&(1u<<id)) || x>=width || y>=height)return Protocol;
        seen|=(uint16_t)(1u<<id); frame.contacts[frame.count++]={(uint8_t)id,(uint16_t)x,(uint16_t)y};
    }
    out=frame; return Ok;
}
inline Result focaltechFrame(const uint8_t *p,size_t bytes,unsigned width,unsigned height,Frame &out) {
    if(!p || bytes<63 || width<2 || height<2)return Invalid;
    unsigned expected=p[2]&15; if(expected>10)return Protocol;
    Frame frame={}; uint16_t seen=0;
    for(unsigned i=0;i<10;i++) {
        const uint8_t *c=p+3+i*6; unsigned id=c[2]>>4,event=c[0]>>6;
        if(id==15)break;
        if(event==1)continue;
        unsigned x=((unsigned)(c[0]&15)<<8)|c[1],y=((unsigned)(c[2]&15)<<8)|c[3];
        if(event==3 || (seen&(1u<<id)) || x>=width || y>=height || frame.count==10)return Protocol;
        seen|=(uint16_t)(1u<<id); frame.contacts[frame.count++]={(uint8_t)id,(uint16_t)x,(uint16_t)y};
    }
    if(frame.count!=expected)return Protocol;
    out=frame; return Ok;
}
struct Debounce {
    uint32_t candidate=0,stable=0; unsigned matches=0;
    bool sample(uint32_t value) {
        if(value!=candidate){candidate=value;matches=1;return false;}
        if(matches<3)matches++;
        if(matches<3 || stable==candidate)return false;
        stable=candidate;return true;
    }
};
}
