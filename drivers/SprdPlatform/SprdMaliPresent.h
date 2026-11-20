// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <string.h>
#include "SprdMaliPresentPayload.h"

namespace SprdMaliPresent {
constexpr uint32_t Width=1200,Height=1920;
constexpr uint32_t SourceStride=4864,DestinationStride=Width*4;
constexpr uint64_t SourceBytes=SourceStride*Height,DestinationBytes=DestinationStride*Height;
constexpr uint64_t DestinationPhysical=0x9e000000,ReservedBytes=0x9e4000;
constexpr uint64_t JobIova=0x04000000,SourceIova=0x04200000,DestinationIova=0x04c00000;
constexpr size_t JobBytes=0x4000;
using namespace SprdMaliPresentPayload;

static_assert(SourceBytes%16384==0 && DestinationBytes%4096==0 && DestinationBytes<=ReservedBytes);
static_assert(JobIova+JobBytes<SourceIova && SourceIova+SourceBytes<DestinationIova);
static_assert(CodeOffset+sizeof(Code)<=JobBytes);

inline bool supported(uint32_t width,uint32_t height,uint64_t sourceStride,uint64_t sourceLength,
    uint64_t physical,uint64_t destinationStride,uint64_t destinationLength)
{
    return width==Width && height==Height && sourceStride==SourceStride && sourceLength>=SourceBytes &&
        physical==DestinationPhysical && destinationStride==DestinationStride &&
        destinationLength==DestinationBytes;
}

inline bool sourceSpan(uint64_t physical,uint64_t bytes)
{
    return physical && bytes && !((physical|bytes)&4095u) &&
        physical<=UINT64_MAX-bytes &&
        (physical<DestinationPhysical ? bytes<=DestinationPhysical-physical :
          physical-DestinationPhysical>=ReservedBytes);
}

inline void initialize(void *packet)
{
    auto *p=static_cast<uint8_t *>(packet);
    memset(p,0,JobBytes);
    memcpy(p+JobOffset,Job,sizeof(Job));
    memcpy(p+StateOffset,State,sizeof(State));
    memcpy(p+StorageOffset,Storage,sizeof(Storage));
    memcpy(p+CodeOffset,Code,sizeof(Code));
}

inline uint32_t countMismatches(const uint32_t *source,const volatile uint32_t *destination)
{
    uint32_t mismatches=0;
    for(uint32_t row=0;row<Height;++row)
        for(uint32_t x=0;x<Width;++x)
            mismatches+=source[row*(SourceStride/4)+x]!=destination[row*(DestinationStride/4)+x];
    return mismatches;
}
}
