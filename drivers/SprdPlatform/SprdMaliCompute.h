// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "SprdMaliComputePayload.h"

namespace SprdMaliCompute {
constexpr uint64_t Gpu=0x03000000;
constexpr size_t Words=4096, DataBytes=Words*sizeof(uint32_t);
constexpr unsigned Passes=2;
constexpr size_t Input=0x4000, Output=0xc000, BufferBytes=0x14000;
constexpr uint32_t Mask=0x51c39a6e;
constexpr uint8_t Guard=0xa5;
using namespace SprdMaliComputePayload;
static_assert(JobOffset+sizeof(Job)<=StateOffset);
static_assert(StateOffset+sizeof(State)<=StorageOffset);
static_assert(StorageOffset+sizeof(Storage)<=CodeOffset);
static_assert(CodeOffset+sizeof(Code)<Input && Input+DataBytes<Output && Output+DataBytes<BufferBytes);

inline uint32_t inputWord(uint32_t i,uint32_t pass)
{
    return ((i+1)*0x9e3779b9u) ^ (0x73b82418u+pass*0x6b117d11u);
}

inline bool prepare(void *buffer,size_t bytes,uint32_t pass)
{
    if(!buffer || bytes!=BufferBytes || pass>=Passes)return false;
    auto *p=static_cast<uint8_t *>(buffer);
    memset(p,Guard,BufferBytes);
    // Mesa packs a zero-filled descriptor page with no resource tables.
    memset(p,0,Input);
    memcpy(p+JobOffset,Job,sizeof(Job));
    memcpy(p+StateOffset,State,sizeof(State));
    memcpy(p+StorageOffset,Storage,sizeof(Storage));
    memcpy(p+CodeOffset,Code,sizeof(Code));
    for(uint32_t i=0;i<Words;++i) {
        const uint32_t word=inputWord(i,pass);
        memcpy(p+Input+i*sizeof(word),&word,sizeof(word));
    }
    return true;
}

struct Check {
    uint32_t mismatches=0, inputChanged=0, guards=0;
    uint32_t firstBad=UINT32_MAX;
};

inline bool verify(const void *buffer,size_t bytes,uint32_t pass,Check &check)
{
    check=Check{};
    if(!buffer || bytes!=BufferBytes || pass>=Passes)return false;
    const auto *p=static_cast<const uint8_t *>(buffer);
    for(uint32_t i=0;i<Words;++i) {
        uint32_t read=0,result=0;
        memcpy(&read,p+Input+i*sizeof(read),sizeof(read));
        memcpy(&result,p+Output+i*sizeof(result),sizeof(result));
        const uint32_t original=inputWord(i,pass);
        check.inputChanged+=read!=original;
        if(result!=(original^Mask)) {
            ++check.mismatches;
            if(check.firstBad==UINT32_MAX)check.firstBad=i;
        }
    }
    // Hardware writes back only the 16-byte status area at the job head.
    for(size_t i=16;i<Input;++i) {
        uint8_t expected=0;
        if(i<sizeof(Job))expected=Job[i];
        else if(i>=StateOffset && i<StateOffset+sizeof(State))expected=State[i-StateOffset];
        else if(i>=StorageOffset && i<StorageOffset+sizeof(Storage))expected=Storage[i-StorageOffset];
        else if(i>=CodeOffset && i<CodeOffset+sizeof(Code))expected=Code[i-CodeOffset];
        check.guards+=p[i]!=expected;
    }
    for(size_t i=Input+DataBytes;i<Output;++i)check.guards+=p[i]!=Guard;
    for(size_t i=Output+DataBytes;i<BufferBytes;++i)check.guards+=p[i]!=Guard;
    return !check.mismatches && !check.inputChanged && !check.guards;
}
}
