// SPDX-License-Identifier: GPL-2.0-only
// A legitimate reply may follow unrelated traffic on the shared WCN FIFO.
#include "../drivers/SprdPlatform/SprdWcnScan.h"
#include "../drivers/SprdPlatform/SprdWcnBluetooth.h"
#include <cstdio>
#include <cstring>
using namespace SprdWcn;

struct Gpio {
    unsigned delayUs=0;
    uint32_t read32(unsigned) { return 1; }
    void delay(unsigned us) { delayUs+=us; }
};
enum Kind { Wifi, Bluetooth, Scan, Malformed, NoReply };
struct Host {
    Kind kind;
    unsigned reads=0,writes=0;
    Result packets(bool write,uint8_t *out,size_t bytes) {
        if(write) { writes++; return Ok; }
        reads++;
        if(bytes!=kReceiveBytes)return Invalid;
        memset(out,0,bytes);
        uint8_t *p=out+4;
        size_t length=12;
        unsigned subtype=10;
        p[0]=0;p[1]=0xff;putLe16(p+2,12);
        if(kind==Malformed) {
            put32(out+bytes-8,1681);
            return Ok;
        }
        if(kind==Wifi && reads==201) {
            p[1]=1;put32(p+4,0x1234);
        } else if(kind==Bluetooth && reads==201) {
            const uint8_t complete[]={4,0x0e,4,1,3,0x0c,0};
            memcpy(p,complete,sizeof complete);
            length=sizeof complete;subtype=5;
        } else if(kind==Scan) {
            if(reads==1) {p[1]=kWifiCmdScan;put32(p+4,0x1234);}
            else if(reads==202) {
                p[0]=1;p[1]=kWifiEventScanDone;
                length=13;putLe16(p+2,13);p[12]=1;
            }
        }
        put32(out,(subtype<<24)|(uint32_t)(length<<7));
        put32(out+4+((length+3)&~size_t(3)),1u<<23);
        put32(out+bytes-8,(uint32_t)length);
        return Ok;
    }
};
int main() {
    bool passed=true;
    {
        Host host{Wifi};Gpio gpio;WifiReply reply;
        Result r=wifiCommand(host,gpio,1,0x1234,nullptr,0,reply);
        bool ok=r==Ok && host.reads==201 && reply.unmatched==200;
        printf("wifi after 200 unrelated packets: result=%d reads=%u pass=%u\n",r,host.reads,ok);
        passed&=ok;
    }
    {
        Host host{Bluetooth};Gpio gpio;HciReply reply;
        Result r=hciCommand(host,gpio,0x0c03,nullptr,0,reply);
        bool ok=r==Ok && host.reads==201 && reply.unmatched==200 && reply.complete;
        printf("HCI after 200 unrelated packets: result=%d reads=%u pass=%u\n",r,host.reads,ok);
        passed&=ok;
    }
    {
        Host host{Scan};Gpio gpio;WifiScanReport report={};
        Result r=wifiScan(host,gpio,0,0x1234,0x421,report);
        bool ok=r==Ok && host.reads==202 && report.responseSeen && report.doneSeen;
        printf("scan done after 200 unrelated packets: result=%d reads=%u pass=%u\n",r,host.reads,ok);
        passed&=ok;
    }
    {
        Host host{Malformed};Gpio gpio;HciReply reply;
        Result r=hciCommand(host,gpio,0x0c03,nullptr,0,reply);
        bool ok=r==Protocol && host.reads==1;
        printf("malformed metadata: result=%d reads=%u pass=%u\n",r,host.reads,ok);
        passed&=ok;
    }
    {
        Host host{NoReply};Gpio gpio;HciReply reply;
        Result r=hciCommand(host,gpio,0x0c03,nullptr,0,reply);
        bool ok=r==Timeout && host.reads==10000 && gpio.delayUs==10000000;
        printf("no HCI response: result=%d reads=%u delay_us=%u pass=%u\n",r,host.reads,gpio.delayUs,ok);
        passed&=ok;
    }
    return passed?0:1;
}
