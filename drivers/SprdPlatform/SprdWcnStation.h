// SPDX-License-Identifier: GPL-2.0-only
// SC2355 fullmac ABI: original 1h190 sprdwl_ng.ko (ade3d19a83d5), checked
// against Hadenix/android_kernel_realme_ums512@c7f66a0. No Apple/QCA RF ABI.
#pragma once
#include "SprdWcnScan.h"
namespace SprdWcn {
constexpr size_t kEthernetMaximum=1514,kStationPeers=32;
struct StationCallbacks {
    void *context=nullptr;
    void (*connected)(void *,bool,unsigned,const uint8_t *,uint8_t,int8_t)=nullptr;
    void (*disconnected)(void *,unsigned)=nullptr;
    void (*ethernet)(void *,const uint8_t *,size_t)=nullptr;
    void (*bluetooth)(void *,const uint8_t *,size_t)=nullptr;
};
struct StationPeer { bool present=false;uint8_t address[6]={}; };
struct WifiStation {
    StationCallbacks callbacks;
    StationPeer peers[kStationPeers];
    uint16_t credits[4]={};
    uint8_t context=0,sequence=0;
    bool creditRequired=true,associated=false;
    unsigned received=0,creditEvents=0,linkEvents=0,dropped=0;

    void clearPeers() {for(auto &p:peers)p={};associated=false;}
    Result addCredits(const uint8_t *flow,bool reset) {
        bool any=false;for(unsigned i=0;i<4;i++)any|=flow[i]!=0;
        for(unsigned i=0;i<4;i++) {
            if(!any && reset)credits[i]=0;
            else {
                // Credits are finite firmware descriptors, not permission to
                // send indefinitely after a malformed/reset indication.
                if((unsigned)credits[i]+flow[i]>4096)return Protocol;
                credits[i]=(uint16_t)(credits[i]+flow[i]);
            }
        }
        creditEvents++;return Ok;
    }
    int availableColor() const {
        if(!creditRequired)return 0;
        for(unsigned i=0;i<4;i++)if(credits[i])return (int)i;
        return -1;
    }
    int peerFor(const uint8_t *destination) const {
        for(unsigned i=0;i<kStationPeers;i++)if(peers[i].present) {
            bool same=true;for(unsigned j=0;j<6;j++)same&=peers[i].address[j]==destination[j];
            if(same)return (int)i;
        }
        // A station sends Ethernet destinations through its AP. The original
        // driver uses the same-context peer when the Ethernet DA is not the AP.
        for(unsigned i=6;i<kStationPeers;i++)if(peers[i].present)return (int)i;
        return -1;
    }
    Result packet(unsigned type,unsigned subtype,const uint8_t *p,size_t bytes) {
        if(type!=0)return Ok;
        if(subtype==5) {if(callbacks.bluetooth)callbacks.bluetooth(callbacks.context,p,bytes);return Ok;}
        if(subtype==10 && bytes && (p[0]&7u)!=3) {
            if(bytes<kCommandHeader)return Protocol;
            const size_t declared=le16(p+2);
            if(declared<kCommandHeader || declared>bytes)return Protocol;
            if((p[0]&7u)!=1)return Ok;
            const uint8_t event=p[1],ctx=p[0]>>5;
            const uint8_t *body=p+kCommandHeader;const size_t length=declared-kCommandHeader;
            if(event==0xb3) {
                if(length!=4)return Protocol;
                return addCredits(body,true);
            }
            if(ctx!=context)return Ok;
            if(event==0xf5) {
                if(length<11 || body[0]!=context || body[2]>=kStationPeers)return Protocol;
                StationPeer &peer=peers[body[2]];
                if(body[1]==0)peer={};
                else if(body[1]==1 || body[1]==2) {
                    peer.present=true;for(unsigned i=0;i<6;i++)peer.address[i]=body[3+i];
                }else return Protocol;
            }else if(event==0x80) {
                if(!length)return Protocol;
                const bool success=body[0]==0 || body[0]==2;
                if(success && length<9)return Protocol;
                if(!success && length<3)return Protocol;
                // Variable association IEs are untrusted firmware lengths.
                if(success && length>9) {
                    size_t at=9;
                    for(unsigned i=0;i<3;i++) {
                        if(length-at<2)return Protocol;
                        const size_t n=le16(body+at);at+=2;
                        if(n>length-at)return Protocol;at+=n;
                    }
                    if(at!=length)return Protocol;
                }
                associated=success;linkEvents++;
                if(callbacks.connected)callbacks.connected(callbacks.context,success,
                    success?0:body[2],success?body+1:nullptr,
                    success?body[7]:0,success?(int8_t)body[8]:0);
            }else if(event==0x81) {
                if(length<2)return Protocol;
                clearPeers();linkEvents++;
                if(callbacks.disconnected)callbacks.disconnected(callbacks.context,le16(body));
            }
            return Ok;
        }
        // Original sdio_hif_ops: command22, log23, Ethernet24. Port23
        // must never be interpreted as Ethernet or as a credit descriptor.
        if(subtype!=12 && !(subtype==10 && bytes && (p[0]&7u)==3))return Ok;
        if(bytes<28)return Protocol;
        const unsigned kind=p[0]&15u,ctx=p[0]>>4;
        if(kind!=2 && kind!=3)return Ok;
        if(kind==3){Result r=addCredits(p+24,false);if(r!=Ok)return r;}
        if(ctx!=context || !associated)return Ok;
        const size_t offset=p[1],length=le16(p+2);
        if(offset<28 || offset>bytes || length>bytes-offset)return Protocol;
        // Fragment/reorder negotiation is not enabled by this station path.
        // A fragment must not be presented to BSD as a complete IP packet.
        if(length<14 || length>kEthernetMaximum || (p[22]&31u)){dropped++;return Ok;}
        received++;
        if(callbacks.ethernet)callbacks.ethernet(callbacks.context,p+offset,length);
        return Ok;
    }
    static Result observe(void *self,unsigned type,unsigned subtype,const uint8_t *p,size_t n) {
        return static_cast<WifiStation *>(self)->packet(type,subtype,p,n);
    }
    PacketObserver observer(){return {this,observe};}
};

struct StationJoin {
    WifiBss bss={};
    uint8_t pmk[32]={};bool hasPmk=false;
};
// Only negotiate implemented RSN suites. Copying an AP's list wholesale can
// select a cipher/AKM the host cannot install or an unsupported mandatory MFP.
inline Result stationSecurity(const WifiBss &bss,uint8_t *ie,size_t &bytes) {
    bytes=0;
    if(!ie || bss.ieLength>sizeof(bss.informationElements))return Invalid;
    if(!(bss.capabilities&0x10))return Ok;
    size_t at=0;
    while(at<bss.ieLength) {
        if(bss.ieLength-at<2)return Protocol;
        const size_t n=bss.informationElements[at+1];
        if(n>bss.ieLength-at-2)return Protocol;
        const uint8_t *p=bss.informationElements+at+2;
        if(bss.informationElements[at]==48) {
            if(n<18 || le16(p)!=1 || le32(p+2)!=0x04ac0f00u)return Invalid;
            size_t pos=6;const size_t pairs=le16(p+pos);pos+=2;
            if(!pairs || pairs>(n-pos)/4)return Protocol;
            bool ccmp=false;for(size_t i=0;i<pairs;i++,pos+=4)ccmp|=le32(p+pos)==0x04ac0f00u;
            if(n-pos<2)return Protocol;
            const size_t akms=le16(p+pos);pos+=2;
            if(!akms || akms>(n-pos)/4)return Protocol;
            bool psk=false;for(size_t i=0;i<akms;i++,pos+=4)psk|=le32(p+pos)==0x02ac0f00u;
            if(!ccmp || !psk)return Invalid;
            if(n-pos==1)return Protocol;
            if(n-pos>=2 && (le16(p+pos)&0x40u))return Invalid;
            const uint8_t selected[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
            for(size_t i=0;i<sizeof(selected);i++)ie[i]=selected[i];
            bytes=sizeof(selected);return Ok;
        }
        at+=n+2;
    }
    return Invalid;
}
inline Result encodeStationConnect(const StationJoin &join,uint8_t (&body)[82],
                                  uint8_t (&ie)[22],size_t &ieBytes) {
    if(!join.bss.ssidLength || join.bss.ssidLength>32 || !wifiFrequency(join.bss.channel))return Invalid;
    Result r=stationSecurity(join.bss,ie,ieBytes);if(r!=Ok)return r;
    for(auto &b:body)b=0;
    if(ieBytes){put32(body,2);body[12]=0x84;body[13]=0x84;body[14]=0x82;}
    if(join.hasPmk && !ieBytes)return Invalid;
    for(unsigned i=0;i<6;i++)body[4+i]=join.bss.bssid[i];
    body[10]=join.bss.channel;body[17]=join.bss.ssidLength;
    if(join.hasPmk){body[16]=32;for(unsigned i=0;i<32;i++)body[18+i]=join.pmk[i];}
    for(unsigned i=0;i<join.bss.ssidLength;i++)body[50+i]=join.bss.ssid[i];
    return Ok;
}
inline Result encodeStationEthernet(const WifiStation &station,const uint8_t *frame,size_t bytes,
                                    uint8_t *out,size_t capacity,size_t &used,int &color,bool &eapol) {
    used=0;color=-1;eapol=false;
    if(!frame || !out || bytes<14 || bytes>kEthernetMaximum)return Invalid;
    if(!station.associated)return Nack;
    const int peer=station.peerFor(frame);if(peer<0)return Nack;
    eapol=frame[12]==0x88 && frame[13]==0x8e;
    color=eapol?0:station.availableColor();if(color<0)return Timeout;
    const size_t descriptor=eapol?16:0,body=eapol?12+descriptor+bytes:11+bytes;
    const size_t eof=4+((body+3)&~size_t(3)),padded=((eof+4+839)/840)*840;
    if(capacity<padded)return Invalid;
    for(size_t i=0;i<padded;i++)out[i]=0;
    put32(out,((eapol?8u:10u)<<24)|(uint32_t)(body<<7));
    uint8_t *p=out+4;
    if(eapol) {
        // Original xmit_data2cmd uses CMD_TX_DATA (72), "01234", and an
        // eleven-byte MSDU descriptor. This path does not spend data credits.
        p[0]=(uint8_t)(0x10|(station.context<<5));p[1]=72;putLe16(p+2,(uint16_t)body);
        for(unsigned i=0;i<5;i++)p[12+i]=(uint8_t)('0'+i);p+=17;
    }
    p[0]=(uint8_t)((eapol?0:2)|(station.context<<5));p[1]=11;
    const bool special=frame[12]==0x08 && frame[13]==0x06;
    p[2]=special?4:0;putLe16(p+3,(uint16_t)bytes);p[6]=(uint8_t)peer;
    const uint16_t seq=eapol?0:(uint16_t)(((uint8_t)(station.sequence+1)<<2)|(unsigned)color);
    putLe16(p+7,seq);
    for(size_t i=0;i<bytes;i++)p[11+i]=frame[i];
    put32(out+eof,1u<<23);
    used=padded;return Ok;
}
}
