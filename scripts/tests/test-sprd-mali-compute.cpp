// SPDX-License-Identifier: GPL-2.0-only
#include "../../drivers/SprdPlatform/SprdMaliCompute.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

namespace Compute=SprdMaliCompute;
namespace {
void putWord(uint8_t *buffer,size_t at,uint32_t value)
{
    memcpy(buffer+at,&value,sizeof(value));
}

void fakeGpuCompletion(uint8_t *buffer,unsigned pass)
{
    for(uint32_t i=0;i<Compute::Words;++i)
        putWord(buffer,Compute::Output+i*sizeof(uint32_t),Compute::inputWord(i,pass)^Compute::Mask);
    putWord(buffer,0,1); // The GPU may write the job status area.
}
}

int main()
{
    alignas(64) uint8_t buffer[Compute::BufferBytes]={};
    Compute::Check check{};
    assert(!Compute::prepare(nullptr,sizeof(buffer),0));
    assert(!Compute::prepare(buffer,sizeof(buffer)-1,0));
    assert(!Compute::prepare(buffer,sizeof(buffer),Compute::Passes));
    assert(!Compute::verify(nullptr,sizeof(buffer),0,check));
    assert(!Compute::verify(buffer,sizeof(buffer)-1,0,check));
    assert(!Compute::verify(buffer,sizeof(buffer),Compute::Passes,check));
    for(unsigned pass=0;pass<Compute::Passes;++pass) {
        assert(Compute::prepare(buffer,sizeof(buffer),pass));
        assert(!Compute::verify(buffer,sizeof(buffer),pass,check));
        assert(check.mismatches && !check.inputChanged && !check.guards && check.firstBad==0);
        for(size_t i=Compute::Output;i<Compute::Output+Compute::DataBytes;++i)
            assert(buffer[i]==Compute::Guard);

        fakeGpuCompletion(buffer,pass);
        assert(Compute::verify(buffer,sizeof(buffer),pass,check));
        assert(check.firstBad==UINT32_MAX);
        constexpr uint32_t bad=Compute::Words-1;
        const uint32_t result=Compute::inputWord(bad,pass)^Compute::Mask;
        putWord(buffer,Compute::Output+bad*sizeof(uint32_t),result^1);
        assert(!Compute::verify(buffer,sizeof(buffer),pass,check));
        assert(check.mismatches==1 && check.firstBad==bad);
        putWord(buffer,Compute::Output+bad*sizeof(uint32_t),result);

        for(const size_t at : {Compute::CodeOffset+size_t{16},
                             Compute::Output-size_t{1},
                             Compute::Output+Compute::DataBytes}) {
            buffer[at]^=1;
            assert(!Compute::verify(buffer,sizeof(buffer),pass,check) && check.guards==1);
            buffer[at]^=1;
        }
        buffer[Compute::Input]^=1;
        assert(!Compute::verify(buffer,sizeof(buffer),pass,check) && check.inputChanged==1);
        buffer[Compute::Input]^=1;
        assert(Compute::verify(buffer,sizeof(buffer),pass,check));
        fakeGpuCompletion(buffer,pass^1);
        assert(!Compute::verify(buffer,sizeof(buffer),pass,check) && check.mismatches==Compute::Words);
    }
    std::puts("PASS: G52 compute readback contract, stale output, guards, and input integrity");
}
