// SPDX-License-Identifier: GPL-2.0-only
#include "../drivers/SprdPlatform/SprdWcnStation.h"
#include "../drivers/SprdPlatform/SprdWcnReceive.h"
#include <cstdio>
#include <cstring>
using namespace SprdWcn;
struct Seen {unsigned links=0,lost=0,rx=0,bt=0;bool success=false;};
void link(void *v,bool good,unsigned,const uint8_t *,uint8_t,int8_t) {
    auto &s=*(Seen *)v;s.links++;s.success=good;
}
void lost(void *v,unsigned){((Seen *)v)->lost++;}
void rx(void *v,const uint8_t *p,size_t n){if(n==60 && p[12]==8)((Seen *)v)->rx++;}
void bt(void *v,const uint8_t *,size_t){((Seen *)v)->bt++;}
size_t message(uint8_t *out,uint8_t event,uint8_t ctx,const uint8_t *body,size_t n,uint32_t transaction=0) {
    memset(out,0,kCommandHeader+n);out[0]=(uint8_t)((event>=0x80?1:0)|(ctx<<5));
    out[1]=event;putLe16(out+2,(uint16_t)(kCommandHeader+n));put32(out+4,transaction);
    if(n)memcpy(out+kCommandHeader,body,n);return kCommandHeader+n;
}
struct Aggregate {
    uint8_t data[kReceiveBytes]={};size_t used=0,valid=0;
    void add(unsigned subtype,const uint8_t *p,size_t n,bool checksum=false) {
        put32(data+used,(subtype<<24)|(uint32_t)(n<<7)|(checksum?64u:0u));
        memcpy(data+used+4,p,n);used+=4+((n+(checksum?2:0)+3)&~size_t(3));valid+=n;
        put32(data+used,1u<<23);put32(data+kReceiveBytes-8,(uint32_t)valid);
    }
};
struct Gpio {unsigned delays=0;uint32_t read32(unsigned){return 0;}void delay(unsigned){delays++;}};
struct Host {
    Aggregate a;unsigned reads=0,writes=0;
    Result packets(bool write,uint8_t *p,size_t n) {
        if(write){writes++;return n==840?Ok:Invalid;}
        if(n!=kReceiveBytes || reads++)return Invalid;memcpy(p,a.data,n);return Ok;
    }
};
int main() {
    bool pass=true;auto check=[&](bool ok,int line){if(!ok)printf("FAIL line %d\n",line);pass&=ok;};WifiStation station;Seen seen;
    station.context=1;station.callbacks={&seen,link,lost,rx,bt};
    uint8_t frame[60]={};frame[0]=2;frame[12]=8;frame[13]=0;
    uint8_t out[kTransmitBytes]={};size_t used=0;int color=-1;bool eapol=false;
    check(encodeStationEthernet(station,frame,sizeof(frame),out,sizeof(out),used,color,eapol)==Nack,__LINE__);
    // A real connect event is independent of the CONNECT acknowledgement.
    uint8_t connect[15]={0,2,1,2,3,4,5,6,0xd0};uint8_t msg[128]={};
    size_t n=message(msg,0x80,1,connect,sizeof(connect));
    check(station.packet(0,10,msg,n)==Ok && station.associated && seen.links==1 && seen.success,__LINE__);
    check(encodeStationEthernet(station,frame,sizeof(frame),out,sizeof(out),used,color,eapol)==Nack,__LINE__);
    const uint8_t lut[]={1,1,7,2,1,2,3,4,5,1,0};n=message(msg,0xf5,1,lut,sizeof(lut));
    check(station.packet(0,10,msg,n)==Ok,__LINE__);
    check(encodeStationEthernet(station,frame,sizeof(frame),out,sizeof(out),used,color,eapol)==Timeout,__LINE__);
    const uint8_t credit[]={0,2,0,0};n=message(msg,0xb3,0,credit,sizeof(credit));
    check(station.packet(0,10,msg,n)==Ok && station.credits[1]==2,__LINE__);
    check(encodeStationEthernet(station,frame,sizeof(frame),out,sizeof(out),used,color,eapol)==Ok,__LINE__);
    check(used==840 && (le32(out)>>24)==10 && out[4]==0x22 && out[5]==11 &&
          le16(out+7)==60 && out[10]==7 && color==1 && !eapol &&
          !memcmp(out+15,frame,sizeof(frame)) && le32(out+76)==0x800000,__LINE__);
    frame[12]=0x88;frame[13]=0x8e;
    check(encodeStationEthernet(station,frame,sizeof(frame),out,sizeof(out),used,color,eapol)==Ok &&
          eapol && (le32(out)>>24)==8 && out[5]==72 && !memcmp(out+16,"01234",5) &&
          !memcmp(out+32,frame,sizeof(frame)),__LINE__);
    frame[12]=8;frame[13]=0;
    // Port23 carries firmware logs. Port24 descriptor offsets/lengths are
    // checked before a complete Ethernet packet reaches the network stack.
    uint8_t data[100]={};data[0]=0x13;data[1]=28;putLe16(data+2,60);memcpy(data+28,frame,60);data[24]=3;
    check(station.packet(0,11,data,88)==Ok && !seen.rx,__LINE__);
    check(station.packet(0,12,data,88)==Ok && seen.rx==1 && station.credits[0]==3,__LINE__);
    data[1]=99;check(station.packet(0,12,data,88)==Protocol,__LINE__);data[1]=28;
    data[22]=0x10;check(station.packet(0,12,data,88)==Ok && seen.rx==1,__LINE__);data[22]=0;
    uint8_t malformed[15]={};malformed[9]=255;n=message(msg,0x80,1,malformed,sizeof(malformed));
    check(station.packet(0,10,msg,n)==Protocol && seen.links==1,__LINE__);
    // Replies followed by unrelated frames in the same FIFO slot must not
    // hide those frames; DTBS continuation must work while GPIO32 is low.
    Host host;Gpio gpio;uint8_t p[128];
    n=message(p,10,1,nullptr,0,91);host.a.add(10,p,n);
    n=message(p,0xb3,0,credit,4);host.a.add(10,p,n);
    const uint8_t h4[]={4,0x0e,4,1,3,12,0};host.a.add(5,h4,sizeof(h4),true);
    bool pending=true;WifiReply reply;
    check(wifiCommand(host,gpio,10,91,nullptr,0,reply,1,station.observer(),&pending)==Ok &&
          host.reads==1 && host.writes==1 && !pending && seen.bt==1 && station.credits[1]==4,__LINE__);
    host.a.data[2]|=0x80;check(observeAggregate(host.a.data,kReceiveBytes,station.observer())==Protocol,__LINE__);
    // Canonical WPA2/CCMP/PSK RSN IE; optional PMK stays raw, never hex.
    StationJoin join;join.bss.ssidLength=4;memcpy(join.bss.ssid,"test",4);join.bss.channel=6;
    uint8_t body[82]={},ie[22]={};size_t ieBytes=0;
    check(encodeStationConnect(join,body,ie,ieBytes)==Ok && ieBytes==0 && body[17]==4,__LINE__);
    const uint8_t rsn[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    join.bss.capabilities=0x10;memcpy(join.bss.informationElements,rsn,sizeof(rsn));join.bss.ieLength=sizeof(rsn);
    join.hasPmk=true;for(unsigned i=0;i<32;i++)join.pmk[i]=(uint8_t)i;
    check(encodeStationConnect(join,body,ie,ieBytes)==Ok && le32(body)==2 && body[12]==0x84 &&
          body[14]==0x82 && body[16]==32 && !memcmp(body+18,join.pmk,32) && ieBytes==22,__LINE__);
    join.bss.informationElements[20]=0x40;check(encodeStationConnect(join,body,ie,ieBytes)==Invalid,__LINE__);
    join.bss.informationElements[20]=0;join.bss.informationElements[8]=255;
    check(encodeStationConnect(join,body,ie,ieBytes)==Protocol,__LINE__);
    const uint8_t reason[]={3,0};n=message(msg,0x81,1,reason,2);
    check(station.packet(0,10,msg,n)==Ok && !station.associated && seen.lost==1 && !station.peers[7].present,__LINE__);
    printf("%s: original station ACK/event, LUT, credits, TX10/RX24, EAPOL72, interleaving, RSN/PMK and bounds\n",pass?"PASS":"FAIL");
    return pass?0:1;
}
