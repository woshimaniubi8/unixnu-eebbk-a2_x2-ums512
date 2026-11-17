// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdWcnScan.h"
#include <cassert>
#include <cstring>

using namespace SprdWcn;

static size_t appendPacket(uint8_t *out,size_t at,const uint8_t *payload,size_t bytes){
    put32(out+at,(10u<<24)|(uint32_t)(bytes<<7));
    for(size_t i=0;i<bytes;i++)out[at+4+i]=payload[i];
    const size_t wire=4+((bytes+3)&~size_t(3));
    return at+wire;
}

static size_t makeEvent(uint8_t *out,uint8_t id,const uint8_t *event,size_t bytes){
    out[0]=1;out[1]=id;putLe16(out+2,(uint16_t)(12+bytes));
    out[8]=0;out[9]=0;out[10]=out[11]=0;
    for(size_t i=0;i<bytes;i++)out[12+i]=event[i];
    return 12+bytes;
}

int main(){
    {
        uint8_t body[32]={};size_t bytes=0;
        assert(encodeWifiScanRequest(body,sizeof body,0x421,nullptr,0,nullptr,0,bytes)==Ok);
        assert(bytes==12 && le32(body)==0x421 && le32(body+4)==0 && le16(body+8)==0 && le16(body+10)==0);
        const uint8_t ssid[]={ 't','e','s','t' };const uint16_t five[]={36,40};
        assert(encodeWifiScanRequest(body,sizeof body,0x421,ssid,sizeof ssid,five,2,bytes)==Ok);
        assert(bytes==20 && le16(body+8)==4 && !memcmp(body+10,ssid,4) &&
               le16(body+14)==2 && le16(body+16)==36 && le16(body+18)==40);
        assert(encodeWifiScanRequest(body,sizeof body,0x421,ssid,33,five,2,bytes)==Invalid);
        assert(encodeWifiScanRequest(body,11,0x421,nullptr,0,nullptr,0,bytes)==Invalid);
    }
    {
        uint8_t rx[kReceiveBytes]={},response[16]={},mgmt[12+45]={},done[1]={1},frame[45]={};
        response[0]=0x10;response[1]=kWifiCmdScan;putLe16(response+2,16);put32(response+4,0x1234);put32(response+12,0);
        frame[0]=0x80;frame[1]=0;frame[16]=0x02;frame[17]=0x11;frame[18]=0x22;frame[19]=0x33;frame[20]=0x44;frame[21]=0x55;
        putLe16(frame+32,100);putLe16(frame+34,0x431);
        frame[36]=0;frame[37]=4;memcpy(frame+38,"test",4);frame[42]=3;frame[43]=1;frame[44]=6;
        mgmt[0]=4;mgmt[1]=6;mgmt[2]=(uint8_t)-53;putLe16(mgmt+10,sizeof frame);memcpy(mgmt+12,frame,sizeof frame);
        uint8_t scanEvent[12+sizeof mgmt]={},doneEvent[13]={};
        size_t scanBytes=makeEvent(scanEvent,kWifiEventMgmtFrame,mgmt,sizeof mgmt);
        size_t doneBytes=makeEvent(doneEvent,kWifiEventScanDone,done,sizeof done);
        size_t at=0;at=appendPacket(rx,at,response,sizeof response);at=appendPacket(rx,at,scanEvent,scanBytes);at=appendPacket(rx,at,doneEvent,doneBytes);
        put32(rx+kReceiveBytes-8,(uint32_t)(sizeof response+scanBytes+doneBytes));put32(rx+kReceiveBytes-4,0);
        WifiScanReport report={};
        assert(decodeWifiScanAggregate(rx,sizeof rx,0x1234,0,report)==Ok);
        assert(report.responseSeen && report.responseStatus==0 && report.responseCode==0);
        assert(report.doneSeen && report.doneType==1 && report.events==2);
        assert(report.scanFrames==1 && report.uniqueNetworks==1 && report.invalidFrames==0);
        const WifiBss &b=report.bss[0];
        assert(b.ssidLength==4 && !memcmp(b.ssid,"test",4) && b.channel==6 && b.frequencyMhz==2437);
        assert(b.signalDbm==-53 && b.beaconInterval==100 && b.capabilities==0x431 && b.ieLength==9);
    }
    {
        uint8_t rx[kReceiveBytes]={},done[1]={0},p[13]={};
        size_t n=makeEvent(p,kWifiEventScanDone,done,1);
        size_t end=appendPacket(rx,0,p,n);put32(rx+kReceiveBytes-8,(uint32_t)n);
        WifiScanReport report={};assert(decodeWifiScanAggregate(rx,sizeof rx,7,0,report)==Protocol);
        put32(rx+kReceiveBytes-8,1681);assert(decodeWifiScanAggregate(rx,sizeof rx,7,0,report)==Protocol);
        assert(decodeWifiScanAggregate(rx,1680,7,0,report)==Invalid);
        (void)end;
    }
    return 0;
}
