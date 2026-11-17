// SPDX-License-Identifier: GPL-2.0-only
// Original 1h190 sprdwl_down_ini_cmd and sprdwl_open_fw wire contracts.
#pragma once
#include "SprdWcnPackets.h"
#include "SprdWcnCalibrationData.h"

namespace SprdWcn {
struct WifiCalibrationSection {uint8_t index;const uint8_t *data;size_t bytes;};
constexpr WifiCalibrationSection kFactoryWifiSections[]={
    {1,kFactoryWifiSection1,sizeof(kFactoryWifiSection1)},
    {2,kFactoryWifiSection2,sizeof(kFactoryWifiSection2)},
    {4,kFactoryWifiSection4,sizeof(kFactoryWifiSection4)}
};

inline uint16_t wifiCalibrationCrc(const uint8_t *data,size_t bytes){
    uint16_t value=0xffff;
    for(size_t i=0;i<bytes;i++){
        value^=data[i];
        for(unsigned bit=0;bit<8;bit++)value=(uint16_t)((value>>1)^((value&1)?0xa001:0));
    }
    return value;
}

inline Result encodeWifiIni(uint8_t *out,size_t capacity,const WifiCalibrationSection &section,size_t &used){
    used=0;
    if(!out || !section.data || !section.bytes || section.bytes>1464 ||
       section.index<1 || section.index>4 || capacity<section.bytes+6)return Invalid;
    put32(out,section.index);
    for(size_t i=0;i<section.bytes;i++)out[4+i]=section.data[i];
    const uint16_t crc=wifiCalibrationCrc(section.data,section.bytes);
    out[4+section.bytes]=(uint8_t)crc;out[5+section.bytes]=(uint8_t)(crc>>8);
    used=section.bytes+6;return Ok;
}

inline Result encodeWifiOpen(uint8_t *out,size_t capacity,const uint8_t *factoryMac,size_t macBytes){
    if(!out || capacity<8 || !factoryMac || macBytes!=6 || (factoryMac[0]&1))return Invalid;
    uint8_t any=0;for(unsigned i=0;i<6;i++)any|=factoryMac[i];
    if(!any)return Invalid;
    out[0]=1;out[1]=0;
    for(unsigned i=0;i<6;i++)out[2+i]=factoryMac[i];
    return Ok;
}
}
