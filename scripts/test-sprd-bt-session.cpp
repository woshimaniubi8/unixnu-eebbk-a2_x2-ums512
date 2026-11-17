#include "../drivers/SprdPlatform/SprdWcnPackets.h"
#include "../drivers/SprdPlatform/SprdWcnSession.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
using namespace SprdBus;
struct Host {
    std::map<uint32_t,uint32_t> cp={{0x4083c10c,0x55},{0x4083c110,0xc80}};
    std::map<unsigned,uint8_t> aon={{0x148,0x66},{0x1f0,0}};
    bool lockStuck=false,ack=true,failWrite=false;unsigned restores=0;
    Result byte(unsigned fn,unsigned reg,uint8_t &v){assert(!fn);v=aon[reg];return Ok;}
    Result put(unsigned fn,unsigned reg,uint8_t v){
        assert(!fn);aon[reg]=v;
        if(reg==0x1d0)aon[0x1f0]&=~v;
        if(reg==0x1b0&&v==4){assert(restores==1);if(ack)aon[0x1f0]|=0x10;}
        return Ok;
    }
    Result readCp(uint32_t a,uint32_t &v){v=a==0x40850800?(lockStuck?1:0):cp[a];return Ok;}
    Result writeCpBytes(uint32_t a,const uint8_t *p,size_t n){
        if(a==0x405a8000){assert(n==0x36300&&p[0]==0xa5);restores++;return failWrite?Protocol:Ok;}
        assert(n==4);cp[a]=SprdWcn::le32(p);return Ok;
    }
    void pause(unsigned){}
};
int main(){
    for(unsigned test=0;test<5;test++){
        Host host;SprdWcn::Session<Host> s(host);s.stage=14;s.bt={0x405a8000,0x36300};
        std::vector<uint8_t> data(s.bt.bytes,0xa5);
        if(test==1)host.lockStuck=true;
        if(test==2)host.ack=false;
        if(test==3)host.failWrite=true;
        if(test==4)s.stage=4;
        Result got=s.openBluetooth(data.data(),data.size());
        assert(got==(test==0?Ok:test==1||test==2?Timeout:test==3?Protocol:Invalid));
        if(!test){assert(s.stage==20&&host.cp[0x4083c110]==0x80&&host.cp[0x4083c10c]==0x55&&host.aon[0x1f0]==0);}
        if(test==1||test==4)assert(!host.restores&&host.cp[0x4083c110]==0xc80);
    }
    puts("PASS: original UMW2652 BT memory bits preserve Wi-Fi, retained snapshot before BT_BIN_DOWNLOAD, BT_OPEN ack and bounded lock/copy/ack faults");
}
