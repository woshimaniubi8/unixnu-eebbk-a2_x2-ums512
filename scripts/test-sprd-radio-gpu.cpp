// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdSdio.h"
#include "../drivers/SprdPlatform/SprdWcnPower.h"
#include "../drivers/SprdPlatform/SprdMali.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
using namespace SprdBus;
struct Io {
    std::map<unsigned,uint32_t> regs;
    bool aliases=false;
    bool gpioData=false;unsigned gpioWrites=0;
    uint32_t read32(unsigned a){return regs[a];}
    void write32(unsigned a,uint32_t v){
        if(gpioData && a%0x80==0){
            gpioWrites++;for(unsigned pin:{97u,143u})if(a==(pin/16)*0x80){
                unsigned bit=1u<<(pin%16);
                assert((regs[a+8]&bit) && !(regs[a+0x28]&bit));
            }
        }
        if(aliases && a>=0x1000 && a<0x2000)regs[a-0x1000]|=v;else if(aliases && a>=0x2000 && a<0x3000)regs[a-0x2000]&=~v;else regs[a]=v;
    }
    void delay(unsigned){}
};
struct Pmic {
    std::map<unsigned,uint16_t> regs;unsigned fail=0;bool failed=false;
    Result read(unsigned a,uint16_t &v){v=regs[a];return Ok;}
    Result write(unsigned a,uint16_t v){
        if(a==fail && !failed){failed=true;return Protocol;}
        if(a==0x1bd0){regs[a]=(uint16_t)((v&0x7fff)|((v&0x7fff)==0x6e7f?0x8000:0));return Ok;}
        if((regs[0x1bd0]&0x7fff)==0x6e7f)regs[a]=v;
        return Ok;
    }
    Result update(unsigned a,uint16_t m,uint16_t v){return write(a,(uint16_t)((regs[a]&~m)|(v&m)));}
};
struct SdioIo {
    std::map<unsigned,uint8_t> bytes,card;
    std::vector<uint32_t> fifo;
    uint32_t status=0,response=0;unsigned portReads=0,commands=0,delays=0;
    bool injectError=false,timeout=false,manfid=true,resetStuck=false;
    uint32_t dmaWord=0;
    unsigned r16(unsigned a){return bytes[a]|bytes[a+1]<<8;}
    uint32_t r32(unsigned a){return r16(a)|(uint32_t)r16(a+2)<<16;}
    SdioIo(){
        card[0]=0x32;card[3]=2;card[9]=0;card[10]=0x10;card[11]=0;
        card[0x1000]=0x20;card[0x1001]=4;for(unsigned i=2;i<6;i++)card[0x1000+i]=0;card[0x1006]=0xff;
    }
    uint8_t read8(unsigned a){return bytes[a];}
    uint16_t read16(unsigned a){return (uint16_t)r16(a);}
    uint32_t read32(unsigned a){
        if(a==0x30)return timeout?0:status;
        if(a==0x10)return response;
        if(a==0x24)return 0;
        if(a==0x20){assert(portReads<fifo.size());return fifo[portReads++];}
        return r32(a);
    }
    void write8(unsigned a,uint8_t v){bytes[a]=a==0x2f && !resetStuck?(uint8_t)(v&~7u):v;}
    void write16(unsigned a,uint16_t v){bytes[a]=(uint8_t)v;bytes[a+1]=(uint8_t)(v>>8);if(a==0x2c && (v&1))bytes[a]|=2;}
    void write32(unsigned a,uint32_t v){
        if(a==0x30){status&=~v;return;}
        write16(a,(uint16_t)v);write16(a+2,(uint16_t)(v>>16));
        if(a!=0xc)return;
        commands++;unsigned cmd=v>>24;uint32_t arg=r32(8);response=0;status=1;
        if(injectError){status=0x8001;return;}
        if(cmd==5)response=0x90ff8000;
        if(cmd==3)response=0x00010000;
        if(cmd==52){
            unsigned fn=(arg>>28)&7,at=(arg>>9)&0x1ffff;assert(fn==0);
            if(arg>>31)card[at]=(uint8_t)arg;
            response=card[at];if(!manfid && at==0x1000)response=0xff;
        }
        if(cmd==53){
            assert(((arg>>28)&7)==1 && ((arg>>9)&0x1ffff)==0xf && (arg&(1u<<26)));
            uint32_t cp=card[0x15c]|(uint32_t)card[0x15d]<<8|(uint32_t)card[0x15e]<<16|(uint32_t)card[0x15f]<<24;
            assert(cp==0x4083c208 || cp==0x408a0000);
            assert((r16(4)&0xfff)==4 && r32(0)==1 && (v&1));
            assert((read8(0x28)&0x18)==0x10 && (read16(0x3e)&0x2c00)==0x2800);
            assert(r32(0x58)==0x90000000 && r32(0x5c)==0);
            dmaWord=cp==0x4083c208?0x23550001u:0x8000u;portReads=0;
            status=1|2|8|0x20; // FIFO must remain unread even if ready latched.
        }
    }
    void delay(unsigned n){delays+=n;}
};
struct DmaMock:SprdSdio::Dma {
    SdioIo &io;unsigned prepared=0,completed=0;bool quarantined=false;
    explicit DmaMock(SdioIo &i):io(i){}
    Result prepare(uint8_t *p,size_t n,bool write,uint64_t &table) override{
        assert(p && n==4 && !write);prepared++;table=0x90000000;return Ok;
    }
    void complete(uint8_t *p,size_t n,bool write,bool success,bool stopped) override{
        assert(p && n==4 && !write);completed++;if(!stopped)quarantined=true;
        if(success)for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(io.dmaWord>>(i*8));
    }
};
struct GpuIo:Io {
    Io *pmu=nullptr,*aon=nullptr;bool fault=false,noReset=false;unsigned poll=0;
    uint32_t read32(unsigned a){
        assert(!(pmu->regs[0x30]&(1u<<25)) && (aon->regs[0]&(1u<<11)));
        if(a==0x20){poll++;return fault?1:(!noReset && poll>=3?0x100:0);}
        return Io::read32(a);
    }
    void write32(unsigned a,uint32_t v){
        assert(!(pmu->regs[0x30]&(1u<<25)));
        if(a==0x30)assert(v==1);Io::write32(a,v);
    }
};
int main(){
    assert(SprdMali::factorySupplyReady(0,224));
    assert(SprdMali::factorySupplyReady(0,256));
    assert(!SprdMali::factorySupplyReady(8,256));
    assert(!SprdMali::factorySupplyReady(0,223));
    assert(!SprdMali::factorySupplyReady(0,64));
    {
        SdioIo io;DmaMock dma(io);SprdSdio::Host<SdioIo> host(io,dma);SprdSdio::Identity id;
        assert(host.initialize()==Ok && host.enumerate(id)==Ok);
        assert(id.vendor==0 && id.device==0 && id.chip==0x23550001 && id.strap==0x8000);
        assert(io.portReads==0 && host.last.transferred==4 && dma.prepared==2 && dma.completed==2);
        unsigned old=io.commands;uint8_t data[513]={};assert(host.extended(false,1,0xf,data,513,true)==Invalid && io.commands==old);
        host.restore();
    }
    {
        SdioIo io;DmaMock dma(io);SprdSdio::Host<SdioIo> host(io,dma);SprdSdio::Identity id;
        assert(host.initialize()==Ok);io.manfid=false;assert(host.enumerate(id)==Protocol);host.restore();
    }
    {
        SdioIo io;DmaMock dma(io);SprdSdio::Host<SdioIo> host(io,dma);assert(host.initialize()==Ok);
        io.injectError=true;uint8_t value=0;assert(host.byte(0,0,value)==Protocol);
        io.injectError=false;io.timeout=true;assert(host.byte(0,0,value)==Timeout);assert(io.delays>=200000);host.restore();
    }
    for(bool stuck:{false,true}){
        SdioIo io;DmaMock dma(io);SprdSdio::Host<SdioIo> host(io,dma);assert(host.initialize()==Ok);
        io.timeout=true;io.resetStuck=stuck;uint8_t data[4]={};
        io.card[0x15c]=8;io.card[0x15d]=0xc2;io.card[0x15e]=0x83;io.card[0x15f]=0x40;
        assert(host.extended(false,1,0xf,data,4,true)==Timeout);
        assert(dma.prepared==1 && dma.completed==1 && dma.quarantined==stuck);
        assert(data[0]==0 && io.portReads==0);host.restore();
    }
    // A transport-successful ADI write may be ignored while PMIC is locked.
    { Pmic locked;locked.regs[0x1bd0]=0;locked.regs[0x1998]=0;
      assert(locked.update(0x1998,0xff,60)==Ok && locked.regs[0x1998]==0); }
    for(unsigned protection:{0u,0xee7fu})for(unsigned failure: {0u,0x1bd0u,0x1998u,0x191cu,0x19d4u,0x1accu}){
        Io gpio,aon,ap,clk,apclk;aon.aliases=ap.aliases=true;gpio.gpioData=true;Pmic pmic;pmic.fail=failure;
        for(unsigned pin:{97u,143u})for(unsigned off:{0u,4u,8u,0x28u})gpio.regs[(pin/16)*0x80+off]=0x55aa55aa;
        aon.regs[0]=0x80000000;ap.regs[0]=0x123;clk.regs[0x228]=0x8855;apclk.regs[0x88]=0xa50003;
        for(unsigned a:{0x1998u,0x1994u,0x1920u,0x191cu,0x19d4u,0x19d0u,0x1ac8u,0x1accu})pmic.regs[a]=0xa001;
        pmic.regs[0x1bd0]=(uint16_t)protection;
        auto gp=gpio.regs,ao=aon.regs,ap0=ap.regs,ck=clk.regs,ac=apclk.regs;auto pm=pmic.regs;
        SprdWcn::Power<Io,Pmic> power(gpio,aon,ap,clk,apclk,pmic);Result r=power.enable();
        assert(r==(failure?Protocol:Ok));
        // The saved state can restore an input pin; enforce the output-only
        // contract during enable(), then check the full restored snapshot.
        gpio.gpioData=false;assert(power.restore()==Ok);
        assert(gpio.regs==gp && aon.regs==ao && ap.regs==ap0 && clk.regs==ck && apclk.regs==ac && pmic.regs==pm);
    }
    for(unsigned mode=0;mode<3;mode++){
        Io pmu,aon,clocks;aon.aliases=true;GpuIo gpu;gpu.pmu=&pmu;gpu.aon=&aon;
        pmu.regs[0x30]=0x82000000;aon.regs[0]=0x80000000;
        for(unsigned a:{4u,8u,12u})clocks.regs[a]=0x1000751;
        gpu.regs[0]=0x72120000;gpu.regs[0x34]=0;gpu.regs[0x28]=0;
        gpu.regs[0x100]=3;gpu.regs[0x110]=1;gpu.regs[0x120]=1;gpu.regs[0x18]=255;gpu.regs[0x1c]=7;
        gpu.fault=mode==1;gpu.noReset=mode==2;auto p=pmu.regs,a=aon.regs,c=clocks.regs;
        SprdMali::Probe<GpuIo> *unused=nullptr;(void)unused;
        // The physical implementation uses one register type for all maps.
        struct Adapter {
            Io *io;GpuIo *g=nullptr;
            uint32_t read32(unsigned o){return g?g->read32(o):io->read32(o);}
            void write32(unsigned o,uint32_t v){if(g)g->write32(o,v);else io->write32(o,v);}
            void delay(unsigned){}
        }pa{&pmu},aa{&aon},ca{&clocks},ga{&gpu,&gpu};
        SprdMali::Probe<Adapter> probe(pa,aa,ca,ga);SprdMali::Identity out;
        assert(probe.run(out)==(mode==0?Ok:mode==1?Protocol:Timeout));probe.restore();
        assert(pmu.regs==p && aon.regs==a && clocks.regs==c);
    }
    puts("PASS: SDIO0/0 identity, original SYSADDR+increment protocol, private ADMA completion without stealing the FIFO, bounded faults, WCN rail/clock/GPIO rollback, Mali reset/fault/timeout and owned-bit restoration");
}
