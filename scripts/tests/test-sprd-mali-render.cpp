// SPDX-License-Identifier: GPL-2.0-only
#include "../../drivers/SprdPlatform/SprdMaliRender.h"
#include <assert.h>
#include <stdio.h>

int main(int argc,char **argv) {
    using namespace SprdMaliRender;
    alignas(64) static uint8_t storage[BufferBytes];
    FILE *capture=argc==2?fopen(argv[1],"wb"):nullptr;
    if(argc==2)assert(capture);
    for(unsigned pass=0;pass<4;pass++) {
        memset(storage,0xa5,sizeof(storage));
        assert(build(storage,sizeof(storage),0x1000000,pass));
        for(size_t i=0x4000;i<sizeof(storage);i++)assert(storage[i]==0xa5);
        if(capture)assert(fwrite(storage,1,sizeof(storage),capture)==sizeof(storage));
    }
    if(capture)assert(fclose(capture)==0);
    assert(!build(nullptr,BufferBytes,0x1000000,0));
    assert(!build(storage,BufferBytes-1,0x1000000,0));
    assert(!build(storage,BufferBytes,0x1000001,0));
    assert(!build(storage,BufferBytes,UINT64_MAX-4095,0));
    assert(!build(storage,BufferBytes,0x1000000,4));
    assert(!build(storage+1,BufferBytes,0x1000000,0));
    puts("PASS: four fragment packet sets, no CPU pixel writes, guard preservation and address/buffer/quadrant bounds");
}
