// SPDX-License-Identifier: MIT
#include "../../drivers/SprdPlatform/SprdMaliPresent.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main()
{
    using namespace SprdMaliPresent;
    assert(supported(Width,Height,SourceStride,SourceBytes,DestinationPhysical,DestinationStride,DestinationBytes));
    assert(!supported(Width,Height,SourceStride,SourceBytes-1,DestinationPhysical,DestinationStride,DestinationBytes));
    assert(!supported(Width,Height,SourceStride,SourceBytes,DestinationPhysical+4096,DestinationStride,DestinationBytes));
    assert(!supported(Width,Height,SourceStride,SourceBytes,DestinationPhysical,DestinationStride,DestinationBytes+4096));
    assert(!supported(Width,Height,DestinationStride,SourceBytes,DestinationPhysical,DestinationStride,DestinationBytes));
    assert(!supported(Height,Width,SourceStride,SourceBytes,DestinationPhysical,DestinationStride,DestinationBytes));
    assert(sourceSpan(0xa4000000,16384));
    assert(sourceSpan(DestinationPhysical-4096,4096));
    assert(!sourceSpan(DestinationPhysical-4096,8192));
    assert(!sourceSpan(DestinationPhysical+DestinationBytes,4096));
    assert(sourceSpan(DestinationPhysical+ReservedBytes,4096));
    assert(!sourceSpan(UINT64_MAX-4095,4096));
    assert(!sourceSpan(0xa4000001,4096));
    assert(!sourceSpan(0,4096));

    std::vector<uint8_t> packet(JobBytes+2,0xa5);
    initialize(packet.data()+1);
    assert(packet.front()==0xa5 && packet.back()==0xa5);
    assert(!memcmp(packet.data()+1+CodeOffset,Code,sizeof(Code)));
    assert(packet[1+CodeOffset+sizeof(Code)]==0);

    std::vector<uint32_t> src(SourceBytes/4,0xa5a5a5a5), dst(DestinationBytes/4+2,0x5a5a5a5a);
    for(uint32_t y=0;y<Height;++y) {
        for(uint32_t x=0;x<Width;++x) {
            const uint32_t pixel=(y<<16)|x;
            src[y*(SourceStride/4)+x]=dst[1+y*Width+x]=pixel;
        }
    }
    assert(countMismatches(src.data(),dst.data()+1)==0);
    dst[DestinationBytes/4]^=1;
    assert(countMismatches(src.data(),dst.data()+1)==1);
    assert(dst.front()==0x5a5a5a5a && dst.back()==0x5a5a5a5a);
    puts("PASS: full-panel Mali present geometry, reserved bounds, padded source and last pixel");
}
