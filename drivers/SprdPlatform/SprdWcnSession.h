// SPDX-License-Identifier: GPL-2.0-only
// Original 1h190 mem_pd_mgr/slp_mgr/sdio_int protocol, also present in
// Unisoc BSP c7f66a0. UMW2652 masks/registers verified in shipped kernel.
#pragma once
#include "SprdBus.h"
#include <stddef.h>
#include <stdint.h>
namespace SprdWcn {
using namespace SprdBus;
struct MemoryRange {uint32_t address=0,bytes=0;};
template<class Host> class Session {
    Host &host;
    Result write(uint32_t a,uint32_t v){return host.writeCpBytes(a,reinterpret_cast<const uint8_t *>(&v),4);}
    Result switchMemory(uint32_t iram,uint32_t dram,bool power){
        Result result=Timeout;uint32_t token=1;
        for(unsigned i=0;i<200;i++){
            result=host.readCp(0x40850800,token);if(result!=Ok)return result;
            if(!token)break;host.pause(10);
        }
        if(token)return Timeout;
        const uint32_t addresses[]={0x4083c10c,0x4083c110},masks[]={iram,dram};
        for(unsigned i=0;i<2 && result==Ok;i++){
            if(!masks[i])continue;uint32_t value=0;
            result=host.readCp(addresses[i],value);
            if(result==Ok)result=write(addresses[i],power?value&~masks[i]:value|masks[i]);
        }
        Result unlock=write(0x40850800,0x55aa10c5);
        return result==Ok?unlock:result;
    }
public:
    uint8_t savedPublicEnable=0,lastPublic=0,lastSleep=0;
    bool publicSaved=false;unsigned stage=0;MemoryRange wifi,bt;
    explicit Session(Host &h):host(h){}
    Result enable(){
        stage=1;Result r=host.byte(0,0x1c0,savedPublicEnable);
        if(r!=Ok)return r;publicSaved=true;
        return host.put(0,0x1c0,(uint8_t)(savedPublicEnable|0x7d));
    }
    Result restore(){return publicSaved?host.put(0,0x1c0,savedPublicEnable):Ok;}
    Result wake(){
        Result r=host.put(0,0x1a2,0);if(r!=Ok)return r;
        for(unsigned i=0;i<100;i++){
            r=host.byte(0,0x148,lastSleep);if(r!=Ok)return r;
            uint8_t state=lastSleep&0xf0;
            if(state!=0 && state!=0x10 && state!=0x20 && state!=0x30 && state!=0x40)return Ok;
            host.pause(50);
        }
        return Timeout;
    }
    Result waitPublic(uint8_t bit,unsigned milliseconds){
        for(unsigned i=0;i<milliseconds;i++){
            Result r=host.byte(0,0x1f0,lastPublic);if(r!=Ok)return r;
            if(lastPublic&bit)return host.put(0,0x1d0,bit);
            host.pause(1000);
        }
        return Timeout;
    }
    Result query(){
        stage=2;Result r=waitPublic(1,2000);if(r!=Ok)return r;
        stage=3;r=wake();if(r!=Ok)return r;
        uint32_t offsets[4]={};
        for(unsigned i=0;i<4;i++){r=host.readCp(0x405e73b4+4*i,offsets[i]);if(r!=Ok)return r;}
        for(unsigned i=0;i<4;i++)if(offsets[i]<0x100000 || offsets[i]>0x200000)return Protocol;
        if(offsets[1]<=offsets[0] || offsets[3]<=offsets[2])return Protocol;
        bt={0x40400000+offsets[0],offsets[1]-offsets[0]};wifi={0x40400000+offsets[2],offsets[3]-offsets[2]};
        if(bt.address<wifi.address+wifi.bytes && wifi.address<bt.address+bt.bytes)return Protocol;
        stage=4;return Ok;
    }
    Result openBluetooth(const uint8_t *snapshot,size_t bytes){
        if(!snapshot || !bt.bytes || bytes!=bt.bytes || stage!=14)return Invalid;
        stage=15;Result r=wake();if(r!=Ok)return r;
        stage=16;r=switchMemory(0,0xc00,true);if(r!=Ok)return r;
        stage=17;r=host.writeCpBytes(bt.address,snapshot,bytes);if(r!=Ok)return r;
        stage=18;r=host.put(0,0x1d0,0x10);if(r!=Ok)return r;
        r=host.put(0,0x1b0,4);if(r!=Ok)return r;
        stage=19;r=waitPublic(0x10,30000);if(r!=Ok)return r;
        stage=20;return wake();
    }
    Result saveAndOpenWifi(uint8_t *wifiBuffer,size_t wifiBytes,uint8_t *btBuffer,size_t btBytes){
        if(!wifiBuffer || !btBuffer || wifiBytes!=wifi.bytes || btBytes!=bt.bytes || !wifiBytes || !btBytes)return Invalid;
        stage=5;Result r=host.readCpBytes(wifi.address,wifiBuffer,wifiBytes);if(r!=Ok)return r;
        stage=6;r=host.readCpBytes(bt.address,btBuffer,btBytes);if(r!=Ok)return r;
        stage=7;r=switchMemory(0xfc00,0xc80,false);if(r!=Ok)return r;
        stage=8;r=host.put(0,0x1b0,8);if(r!=Ok)return r;
        stage=9;r=wake();if(r!=Ok)return r;
        stage=10;r=switchMemory(0xfc00,0x80,true);if(r!=Ok)return r;
        stage=11;r=host.writeCpBytes(wifi.address,wifiBuffer,wifiBytes);if(r!=Ok)return r;
        stage=12;r=host.put(0,0x1d0,8);if(r!=Ok)return r;
        r=host.put(0,0x1b0,2);if(r!=Ok)return r;
        stage=13;r=waitPublic(8,30000);if(r!=Ok)return r;
        stage=14;return wake();
    }
};
}
