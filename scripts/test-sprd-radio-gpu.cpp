// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdSdio.h"
#include "../drivers/SprdPlatform/SprdWcnPackets.h"
#include "../drivers/SprdPlatform/SprdWcnCalibration.h"
#include "../drivers/SprdPlatform/SprdWcnSession.h"
#include "../drivers/SprdPlatform/SprdWcnPower.h"
#include "../drivers/SprdPlatform/SprdMali.h"
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <iterator>
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
    std::map<uint32_t,uint8_t> cpMemory;
    std::vector<uint32_t> fifo;
    std::vector<uint8_t> packetTx,packetRx;
    uint32_t status=0,response=0;unsigned portReads=0,commands=0,delays=0;
    bool injectError=false,timeout=false,manfid=true,resetStuck=false;
    uint32_t currentCp=0;bool currentWrite=false,currentPacket=false;
    unsigned r16(unsigned a){return bytes[a]|bytes[a+1]<<8;}
    uint32_t r32(unsigned a){return r16(a)|(uint32_t)r16(a+2)<<16;}
    SdioIo(){
        card[0]=0x32;card[3]=2;card[9]=0;card[10]=0x10;card[11]=0;
        card[0x1000]=0x20;card[0x1001]=4;for(unsigned i=2;i<6;i++)card[0x1000+i]=0;card[0x1006]=0xff;
        const uint32_t chip=0x23550001,strap=0x8000;
        for(unsigned i=0;i<4;i++){cpMemory[0x4083c208+i]=(uint8_t)(chip>>(i*8));cpMemory[0x408a0000+i]=(uint8_t)(strap>>(i*8));}
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
            assert(((arg>>28)&7)==1);
            currentPacket=((arg>>9)&0x1ffff)==0x20;
            if(currentPacket){
                assert((arg&(1u<<27)) && !(arg&(1u<<26)));
                assert((r16(4)&0xfff)==840 && r32(0)==(arg&511));
                assert((v&0x23)==(r32(0)>1?0x23u:3u));
            }else assert(((arg>>9)&0x1ffff)==0xf && (arg&(1u<<26)));
            uint32_t cp=card[0x15c]|(uint32_t)card[0x15d]<<8|(uint32_t)card[0x15e]<<16|(uint32_t)card[0x15f]<<24;
            currentCp=cp;currentWrite=(arg>>31)!=0;
            assert((r16(4)&0xfff)>0 && (currentPacket || r32(0)==1) && (v&1));
            assert((read8(0x28)&0x18)==0x10 && (read16(0x3e)&0x2c00)==0x2800);
            assert(r32(0x58)==0x90000000 && r32(0x5c)==0);
            portReads=0;
            status=1|2|8|0x20; // FIFO must remain unread even if ready latched.
        }
    }
    void delay(unsigned n){delays+=n;}
};
struct DmaMock:SprdSdio::Dma {
    SdioIo &io;unsigned prepared=0,completed=0;bool quarantined=false;
    explicit DmaMock(SdioIo &i):io(i){}
    Result prepare(uint8_t *p,size_t n,bool,uint64_t &table) override{
        assert(p && n>0 && n<=0x4000);prepared++;table=0x90000000;return Ok;
    }
    void complete(uint8_t *p,size_t n,bool write,bool success,bool stopped) override{
        assert(p && n>0 && n<=0x4000);completed++;if(!stopped)quarantined=true;
        if(success && io.currentPacket){
            if(write)io.packetTx.assign(p,p+n);
            else{assert(io.packetRx.size()==n);std::copy(io.packetRx.begin(),io.packetRx.end(),p);}
            return;
        }
        if(success && write)for(size_t i=0;i<n;i++)io.cpMemory[io.currentCp+i]=p[i];
        if(success && !write)for(size_t i=0;i<n;i++)p[i]=io.cpMemory[io.currentCp+i];
        if(success){
            uint32_t next=io.currentCp+static_cast<uint32_t>(n);
            for(unsigned i=0;i<4;i++)io.card[0x15c+i]=(uint8_t)(next>>(i*8));
        }
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
struct PacketHost {
    unsigned reads=0,writes=0;bool stale=false,fail=false;
    Result packets(bool write,uint8_t *data,size_t bytes){
        if(fail)return Protocol;
        if(write){writes++;assert(bytes==840 && SprdWcn::le32(data+8)==17);return Ok;}
        reads++;assert(bytes==2520);std::fill(data,data+bytes,0);
        SprdWcn::put32(data,(10u<<24)|(16u<<7));data[4]=8;data[5]=9;data[6]=16;
        SprdWcn::put32(data+8,stale?18:17);SprdWcn::put32(data+16,1);
        SprdWcn::put32(data+20,1u<<23);SprdWcn::put32(data+2512,16);return Ok;
    }
};
struct PacketGpio {bool high=true;unsigned delays=0;uint32_t read32(unsigned a){assert(a==0x300);return high?1:0;}void delay(unsigned n){assert(n==1000);delays++;}};
struct SessionHost {
    std::map<uint32_t,uint32_t> cp={{0x405e73b4,0x100000},{0x405e73b8,0x108000},{0x405e73bc,0x110000},{0x405e73c0,0x120000}};
    std::map<unsigned,uint8_t> aon={{0x1c0,0x80},{0x148,0x80},{0x1f0,1}};
    bool lockStuck=false,failCopy=false;unsigned copies=0,restores=0,pauses=0;
    Result byte(unsigned fn,unsigned reg,uint8_t &v){assert(fn==0);v=aon[reg];return Ok;}
    Result put(unsigned fn,unsigned reg,uint8_t v){
        assert(fn==0);aon[reg]=v;if(reg==0x1d0)aon[0x1f0]&=~v;
        if(reg==0x1b0 && v==2){assert(restores==1);aon[0x1f0]|=8;}
        return Ok;
    }
    Result readCp(uint32_t a,uint32_t &v){v=a==0x40850800?(lockStuck?1:0):cp[a];return Ok;}
    Result readCpBytes(uint32_t a,uint8_t *p,size_t n){
        assert((a==0x40510000 && n==0x10000)||(a==0x40500000 && n==0x8000));
        copies++;std::fill(p,p+n,0xa5);return failCopy?Protocol:Ok;
    }
    Result writeCpBytes(uint32_t a,const uint8_t *p,size_t n){
        if(a==0x40510000){assert(n==0x10000 && p[0]==0xa5 && copies==2);restores++;return Ok;}
        assert(n==4);cp[a]=SprdWcn::le32(p);return Ok;
    }
    void pause(unsigned us){pauses+=us;}
};
int main(){
    {
        using namespace SprdWcn;
        assert(wifiCalibrationCrc((const uint8_t *)"123456789",9)==0x4b37);
        const uint16_t expected[]={0x84cb,0xa057,0x4653};
        uint8_t body[1470]={},tx[1680]={};size_t bytes=0,wire=0;unsigned at=0;
        for(const auto &section:kFactoryWifiSections){
            assert(wifiCalibrationCrc(section.data,section.bytes)==expected[at++]);
            assert(encodeWifiIni(body,sizeof(body),section,bytes)==Ok && bytes==section.bytes+6);
            assert(le32(body)==section.index && le16(body+bytes-2)==expected[at-1]);
            assert(std::equal(section.data,section.data+section.bytes,body+4));
            assert(encodeWifiCommand(tx,sizeof(tx),76,at,body,bytes,wire)==Ok);
            assert(wire==(section.index==2?1680:840));
            assert(le16(tx+6)==bytes+12 && std::equal(body,body+bytes,tx+16));
            assert(le32(tx+4+((bytes+15)&~size_t(3)))==(1u<<23));
        }
        assert(encodeWifiIni(body,1469,kFactoryWifiSections[1],bytes)==Invalid && bytes==0);
        uint8_t large[1661]={};std::fill(tx,tx+sizeof(tx),0x5a);
        assert(encodeWifiCommand(tx,1679,76,1,large,1470,wire)==Invalid && wire==0 && tx[0]==0x5a);
        assert(encodeWifiCommand(tx,sizeof(tx),76,1,large,1660,wire,7)==Ok && wire==1680 && tx[4]==0xf0);
        assert(le32(tx+1676)==(1u<<23));
        assert(encodeWifiCommand(tx,sizeof(tx),76,1,large,1661,wire)==Invalid && wire==0);
        assert(encodeWifiCommand(tx,sizeof(tx),76,1,nullptr,0,wire,8)==Invalid);
        const uint8_t unicast[6]={2,1,2,3,4,5},multicast[6]={1,1,2,3,4,5},zero[6]={};
        assert(encodeWifiOpen(body,sizeof(body),unicast,6)==Ok && body[0]==1 && !body[1]);
        assert(std::equal(unicast,unicast+6,body+2));
        assert(encodeWifiOpen(body,sizeof(body),multicast,6)==Invalid);
        assert(encodeWifiOpen(body,sizeof(body),zero,6)==Invalid);
        assert(encodeWifiOpen(body,sizeof(body),nullptr,6)==Invalid);
        assert(encodeWifiOpen(body,sizeof(body),unicast,5)==Invalid);
    }
    {
        SessionHost host;SprdWcn::Session<SessionHost> session(host);
        assert(session.enable()==Ok && host.aon[0x1c0]==0xfd);
        assert(session.query()==Ok && session.wifi.address==0x40510000 && session.bt.bytes==0x8000);
        std::vector<uint8_t> wifi(session.wifi.bytes),bt(session.bt.bytes);
        assert(session.saveAndOpenWifi(wifi.data(),wifi.size(),bt.data(),bt.size())==Ok);
        assert(session.stage==14 && host.copies==2 && host.restores==1);
        assert(host.cp[0x4083c10c]==0 && host.cp[0x4083c110]==0xc00);
        assert(session.restore()==Ok && host.aon[0x1c0]==0x80);
    }
    {
        SessionHost host;SprdWcn::Session<SessionHost> session(host);
        host.aon[0x1f0]=0;assert(session.query()==Timeout);
        host.aon[0x1f0]=1;host.cp[0x405e73bc]=0x104000;assert(session.query()==Protocol);
        host.cp[0x405e73bc]=0x110000;host.aon[0x1f0]=1;assert(session.query()==Ok);
        std::vector<uint8_t> wifi(session.wifi.bytes),bt(session.bt.bytes);
        host.lockStuck=true;assert(session.saveAndOpenWifi(wifi.data(),wifi.size(),bt.data(),bt.size())==Timeout && host.restores==0);
        host.failCopy=true;assert(session.saveAndOpenWifi(wifi.data(),wifi.size(),bt.data(),bt.size())==Protocol && session.stage==5);
        assert(session.saveAndOpenWifi(wifi.data(),wifi.size()-1,bt.data(),bt.size())==Invalid);
        host.aon[0x148]=0x30;assert(session.wake()==Timeout);
    }
    {
        PacketHost host;PacketGpio gpio;SprdWcn::WifiReply reply;
        assert(SprdWcn::wifiCommand(host,gpio,9,17,nullptr,0,reply)==Ok && reply.length==4 && host.reads==1);
        host.stale=true;reply={};assert(SprdWcn::wifiCommand(host,gpio,9,17,nullptr,0,reply)==Protocol && reply.unmatched==64);
        host.stale=false;gpio.high=false;assert(SprdWcn::wifiCommand(host,gpio,9,17,nullptr,0,reply)==Timeout);
        gpio.high=true;host.fail=true;assert(SprdWcn::wifiCommand(host,gpio,9,17,nullptr,0,reply)==Protocol);
    }
    {
        using namespace SprdWcn;
        uint8_t tx[840]={},rx[2520]={},payload[260]={};size_t bytes=0;
        put32(payload,1);
        assert(encodeWifiCommand(tx,sizeof(tx),9,0x12345678,payload,sizeof(payload),bytes)==Ok);
        assert(bytes==840 && le32(tx)==((8u<<24)|(272u<<7)));
        assert(tx[4]==0x10 && le16(tx+6)==272 && le32(tx+8)==0x12345678);
        assert(le32(tx+276)==(1u<<23));
        assert(encodeWifiCommand(tx,839,9,0,nullptr,0,bytes)==Invalid);
        std::copy(tx,tx+840,rx);put32(rx,(10u<<24)|(272u<<7));rx[4]=8;
        put32(rx+2512,272);put32(rx+2516,0);
        WifiReply reply;bool matched=false;
        assert(decodeWifiReply(rx,sizeof(rx),9,0x12345678,reply,matched)==Ok && matched);
        assert(reply.length==260 && le32(reply.payload)==1 && reply.frames==1);
        WifiReply stale;assert(decodeWifiReply(rx,sizeof(rx),9,2,stale,matched)==Ok && !matched && stale.unmatched==1);
        rx[12]=0xff;assert(decodeWifiReply(rx,sizeof(rx),9,0x12345678,reply,matched)==Protocol);
        rx[12]=1;assert(decodeWifiReply(rx,sizeof(rx),9,0x12345678,reply,matched)==Protocol);
        rx[12]=0;rx[6]=0xff;rx[7]=0xff;
        assert(decodeWifiReply(rx,sizeof(rx),9,0x12345678,reply,matched)==Protocol);
        put32(rx+2512,1681);assert(decodeWifiReply(rx,sizeof(rx),9,0,reply,matched)==Protocol);
        put32(rx+2512,0);assert(decodeWifiReply(rx,sizeof(rx),9,0,reply,matched)==Ok && !matched);
        assert(decodeWifiReply(rx,1680,9,0,reply,matched)==Invalid);
    }
    {
        SdioIo io;DmaMock dma(io);SprdSdio::Host<SdioIo> host(io,dma);
        assert(host.initialize()==Ok);uint8_t packet[840]={1,2,3},rx[2520]={};
        assert(host.packets(true,packet,sizeof(packet))==Ok && io.packetTx.size()==840);
        io.packetRx.assign(2520,0xa5);assert(host.packets(false,rx,sizeof(rx))==Ok && rx[2519]==0xa5);
        assert(io.portReads==0 && host.last.argument==0x18004003 && host.last.transferred==2520);
        unsigned old=io.commands;assert(host.packets(true,packet,512)==Invalid && io.commands==old);
        host.restore();
    }
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
        const uint8_t firmware[1024]={0xa0,0x0a,0x1a,0x00,0x19,0x03,0x10,0x00};
        assert(host.writeCpBytes(0x40500000,firmware,sizeof(firmware))==Ok);
        uint8_t readback[1024]={};assert(host.readCpBytes(0x40500000,readback,sizeof(readback))==Ok);
        assert(std::equal(std::begin(firmware),std::end(firmware),std::begin(readback)));
        assert(dma.prepared==6 && dma.completed==6);
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
