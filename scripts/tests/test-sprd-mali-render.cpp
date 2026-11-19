// SPDX-License-Identifier: GPL-2.0-only
#include "../../drivers/SprdPlatform/SprdMaliRender.h"
#include "../../drivers/SprdPlatform/SprdMaliFullFrame.h"
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
    assert(!build(nullptr,BufferBytes,0x1000000,0));
    assert(!build(storage,BufferBytes-1,0x1000000,0));
    assert(!build(storage,BufferBytes,0x1000001,0));
    assert(!build(storage,BufferBytes,UINT64_MAX-4095,0));
    assert(!build(storage,BufferBytes,0x1000000,4));
    assert(!build(storage+1,BufferBytes,0x1000000,0));
    puts("PASS: four fragment packet sets, no CPU pixel writes, guard preservation and address/buffer/quadrant bounds");

    namespace Frame=SprdMaliFullFrame;
    alignas(64) static uint32_t full[Frame::BufferBytes/4];
    memset(full,0xa5,sizeof(full));
    assert(!Frame::build(full,sizeof(full)-1,0x2000000,0));
    assert(!Frame::build(full,sizeof(full),0x2000001,0));
    assert(!Frame::build(full,sizeof(full),UINT64_MAX-4095,0));
    assert(!Frame::build(full,sizeof(full),0x2000000,Frame::Passes));
    assert(!Frame::build(full+1,sizeof(full),0x2000000,0));
    for(unsigned pass=0;pass<Frame::Passes;++pass) {
        memset(full,0xa5,sizeof(full));
        assert(Frame::build(full,sizeof(full),0x2000000,pass));
        for(size_t i=Frame::HeapEnd/4;i<sizeof(full)/4;++i)assert(full[i]==0xa5a5a5a5);
        if(capture)assert(fwrite(full,1,0x800,capture)==0x800);
    }
    auto *image=full+Frame::ImageOffset/4;
    for(unsigned y=0;y<1920;++y)
        for(unsigned x=0;x<1200;++x)image[y*(4864/4)+x]=0xffb3812d;
    Frame::Check check{};
    assert(Frame::verify(full,sizeof(full),0,check));
    assert(!Frame::verify(full,sizeof(full),1,check));
    assert(check.pixels==496*784 && !check.padding && !check.guards);
    for(unsigned y=1136;y<1920;++y)
        for(unsigned x=704;x<1200;++x)image[y*(4864/4)+x]=0xff35cc92;
    assert(Frame::verify(full,sizeof(full),1,check));
    image[1199+1919*(4864/4)]=0;
    image[1200]=0;
    full[Frame::HeapEnd/4]=full[Frame::BufferBytes/4-1]=0;
    assert(!Frame::verify(full,sizeof(full),1,check));
    assert(check.pixels==1 && check.padding==1 && check.guards==2);
    if(capture)assert(fclose(capture)==0);
    puts("PASS: 1200x1920 linear target, partial bounds, row padding and both frame guards");
}
