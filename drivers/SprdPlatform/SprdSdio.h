// SPDX-License-Identifier: GPL-2.0-only
// SDIO command semantics: Linux mmc/core/sdio*.c and sdhci-sprd.c.
// This is the Wi-Fi host at0x71200000, never the eMMC host at0x71400000.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "SprdBus.h"

namespace SprdSdio {
using SprdBus::Result;using SprdBus::Ok;using SprdBus::Invalid;
using SprdBus::Timeout;using SprdBus::Protocol;
struct Identity {
    uint32_t ocr=0,chip=0,strap=0;
    uint16_t rca=0,vendor=0,device=0;
    uint8_t functions=0,cccr=0,ready=0;
};
struct CommandState {unsigned index=0;uint32_t argument=0,status=0,present=0,response=0;size_t transferred=0;};

template<class IO> class Host {
    IO &io;bool started=false;uint16_t oldClock=0,oldControl2=0;
    uint8_t oldControl=0,oldPower=0,oldTimeout=0;
    uint32_t oldIntEnable=0,oldSignal=0;
    bool waitClear(unsigned off,uint32_t mask,unsigned us) {
        while(us--){if(!(io.read32(off)&mask))return true;io.delay(1);}return false;
    }
public:
    CommandState last;
    explicit Host(IO &registers):io(registers){}
    Result clock(uint16_t divisor) {
        // The selected source is ext26m. Identification uses /128 or slower
        // on SDHCR11's 2x path; no guessed PLL or tuning value is installed.
        io.write16(0x2c,0);io.write16(0x2c,(uint16_t)(divisor|1));
        for(unsigned i=0;i<20000;i++){
            if(io.read16(0x2c)&2){io.write16(0x2c,(uint16_t)(divisor|7));return Ok;}
            io.delay(1);
        }return Timeout;
    }
    Result initialize() {
        if(started)return Invalid;
        oldClock=io.read16(0x2c);oldControl2=io.read16(0x3e);
        oldControl=io.read8(0x28);oldPower=io.read8(0x29);oldTimeout=io.read8(0x2e);
        oldIntEnable=io.read32(0x34);oldSignal=io.read32(0x38);started=true;
        // Spreadtrum reset bit3 must stay asserted: clearing it resets card HW.
        io.write8(0x2f,(uint8_t)(io.read8(0x2f)|9));
        for(unsigned i=0;i<20000;i++){
            if(!(io.read8(0x2f)&1)){
                io.write32(0x38,0);io.write32(0x34,0x1fffffff);
                io.write32(0x30,0xffffffff);io.write8(0x2e,0xe);
                io.write16(0x3e,(uint16_t)((oldControl2&~0xfu)|0x3000));
                io.write8(0x28,(uint8_t)(oldControl&~0x3eu));
                io.write8(0x29,0xf);return clock(0x4000);
            }io.delay(1);
        }return Timeout;
    }
    Result command(unsigned index,uint32_t argument,uint16_t flags,
                   uint32_t &response,uint8_t *data=nullptr,size_t length=0,bool write=false) {
        if(index>63 || (length && (!data || length>512)) || (data && !length))return Invalid;
        last={};last.index=index;last.argument=argument;
        if(!waitClear(0x24,3,100000)){last.present=io.read32(0x24);return Timeout;}
        io.write32(0x30,0xffffffff);
        if(length){io.write32(0,1);io.write16(4,(uint16_t)length);flags|=0x20;}
        io.write32(8,argument);
        io.write32(0xc,((uint32_t)index<<24)|((uint32_t)flags<<16)|(length&&!write?0x10u:0u));
        bool commandDone=false;
        for(unsigned t=0;t<200000;t++){
            uint32_t status=io.read32(0x30);last.status=status;
            if(status&0x1fff8000u){last.present=io.read32(0x24);return Protocol;}
            if(status&1){commandDone=true;response=io.read32(0x10);last.response=response;io.write32(0x30,1);}
            // Drain/fill every word before accepting TRANSFER_COMPLETE. Both
            // readiness and completion can be latched in the same snapshot.
            const uint32_t ready=write?0x10u:0x20u;
            if(length && last.transferred<length && (status&ready)){
                while(last.transferred<length){
                    size_t at=last.transferred;uint32_t word=0;
                    if(write){for(unsigned j=0;j<4 && at+j<length;j++)word|=(uint32_t)data[at+j]<<(8*j);io.write32(0x20,word);}
                    else {word=io.read32(0x20);for(unsigned j=0;j<4 && at+j<length;j++)data[at+j]=(uint8_t)(word>>(8*j));}
                    last.transferred+=length-at<4?length-at:4;
                }io.write32(0x30,ready);
            }
            if(commandDone && (!length || ((status&2) && last.transferred==length))){
                if(length)io.write32(0x30,2);
                if((flags&3)==3 && !waitClear(0x24,2,100000))return Timeout;
                return Ok;
            }io.delay(1);
        }last.present=io.read32(0x24);return Timeout;
    }
    Result direct(bool write,unsigned function,unsigned address,uint8_t &value) {
        if(function>7 || address>0x1ffff)return Invalid;
        uint32_t response=0,arg=(write?0x80000000u:0)|function<<28|address<<9|(write?value:0);
        Result r=command(52,arg,0x1a,response);if(r!=Ok)return r;
        if(response&0xcb00)return Protocol;value=(uint8_t)response;return Ok;
    }
    Result byte(unsigned fn,unsigned address,uint8_t &value){return direct(false,fn,address,value);}
    Result put(unsigned fn,unsigned address,uint8_t value){return direct(true,fn,address,value);}
    Result extended(bool write,unsigned fn,unsigned address,uint8_t *data,size_t bytes,bool increment) {
        if(!data || !bytes || bytes>512 || fn>7 || address>0x1ffff)return Invalid;
        uint32_t response=0,arg=(write?0x80000000u:0)|fn<<28|(increment?1u<<26:0)|address<<9|(bytes&511);
        Result r=command(53,arg,0x1a,response,data,bytes,write);
        return r!=Ok?r:(response&0xcb00?Protocol:Ok);
    }
    Result readCp(uint32_t address,uint32_t &value) {
        // Shipped sdiohal_dt_set_addr(): function0 SYSADDR0..3, then
        // function1 direct-mode port0x0f. APB/CIS pointers are not CPU MMIO.
        for(unsigned i=0;i<4;i++){Result r=put(0,0x15c+i,(uint8_t)(address>>(8*i)));if(r!=Ok)return r;}
        uint8_t bytes[4]={};Result r=extended(false,1,0xf,bytes,4,true);
        if(r==Ok)value=(uint32_t)bytes[0]|(uint32_t)bytes[1]<<8|(uint32_t)bytes[2]<<16|(uint32_t)bytes[3]<<24;
        return r;
    }
    Result enumerate(Identity &id) {
        uint32_t response=0;Result r=command(0,0,0,response);if(r!=Ok)return r;
        r=command(5,0,2,id.ocr);if(r!=Ok)return r;
        id.functions=(uint8_t)((id.ocr>>28)&7);
        if(!id.functions || !(id.ocr&0x00ff8000))return Protocol;
        for(unsigned i=0;i<1000;i++){
            r=command(5,id.ocr&0x00ff8000,2,response);if(r!=Ok)return r;
            if(response&0x80000000u)break;
            if(i==999)return Timeout;io.delay(1000);
        }
        r=command(3,0,0x1a,response);if(r!=Ok)return r;id.rca=(uint16_t)(response>>16);if(!id.rca)return Protocol;
        r=command(7,(uint32_t)id.rca<<16,0x1b,response);if(r!=Ok)return r;
        if((r=byte(0,0,id.cccr))!=Ok)return r;
        uint8_t en=0;if((r=byte(0,2,en))!=Ok)return r;if((r=put(0,2,(uint8_t)(en|2)))!=Ok)return r;
        for(unsigned i=0;i<1000;i++){
            if((r=byte(0,3,id.ready))!=Ok)return r;
            if(id.ready&2)break;if(i==999)return Timeout;io.delay(1000);
        }
        uint8_t pointer[3]={};for(unsigned i=0;i<3;i++)if((r=byte(0,9+i,pointer[i]))!=Ok)return r;
        uint32_t at=(uint32_t)pointer[0]|(uint32_t)pointer[1]<<8|(uint32_t)pointer[2]<<16;
        if(at>0x1ffff)return Protocol;
        unsigned consumed=0;bool manfid=false;
        while(consumed<512){
            uint8_t code=0,len=0;if(at>0x1ffff)return Protocol;if((r=byte(0,at++,code))!=Ok)return r;consumed++;
            if(code==0xff)break;if(!code)continue;
            if(at>0x1ffff)return Protocol;
            if((r=byte(0,at++,len))!=Ok)return r;
            consumed++;if(len>512-consumed || at+len>0x20000)return Protocol;
            if(code==0x20){
                if(len<4)return Protocol;uint8_t p[4];for(unsigned j=0;j<4;j++)if((r=byte(0,at+j,p[j]))!=Ok)return r;
                id.vendor=(uint16_t)(p[0]|p[1]<<8);id.device=(uint16_t)(p[2]|p[3]<<8);
                manfid=true;
            }at+=len;consumed+=len;
        }
        // The shipped driver's SDIO_DEVICE(0,0) is intentional. Zero is the
        // actual ROM identity; an absent MANFID tuple is a separate failure.
        if(!manfid || id.vendor || id.device)return Protocol;
        // Original public bus uses840-byte blocks, but the identification
        // operation above uses byte-mode and does not change bus width/speed.
        if((r=put(0,0x110,0x48))!=Ok || (r=put(0,0x111,3))!=Ok)return r;
        if((r=readCp(0x4083c208,id.chip))!=Ok)return r;
        return readCp(0x408a0000,id.strap);
    }
    void restore() {
        if(!started)return;
        io.write32(0x38,0);io.write16(0x2c,0);io.write8(0x29,oldPower);
        io.write8(0x28,oldControl);io.write16(0x3e,oldControl2);io.write8(0x2e,oldTimeout);
        io.write32(0x34,oldIntEnable);io.write32(0x38,oldSignal);io.write16(0x2c,oldClock);started=false;
    }
};
}
