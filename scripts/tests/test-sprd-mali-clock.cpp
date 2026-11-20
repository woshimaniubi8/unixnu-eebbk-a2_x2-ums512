// SPDX-License-Identifier: GPL-2.0-only
#include "../../drivers/SprdPlatform/SprdMali.h"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace SprdMali;
    ClockProfile slow,work;
    assert(clockProfile(26,slow) && clockProfile(384,work));
    assert(slow.coreWord==1 && slow.memoryWord==1 && slow.systemWord==1);
    assert(work.coreMHz==384 && work.memoryMHz==384 && work.systemMHz==192);
    assert(((work.coreWord>>8)&7)==1 && ((work.memoryWord>>8)&7)==1);
    assert(((work.coreWord>>4)&7)==0 && ((work.memoryWord>>4)&7)==0);
    assert(((work.systemWord>>4)&7)==1);
    assert((work.coreWord&1) && (work.memoryWord&1) && (work.systemWord&1));
    assert(!clockProfile(0,work) && !clockProfile(512,work) && !clockProfile(UINT32_MAX,work));
    assert(work.coreMHz==384 && work.coreWord==0x101);
    assert(!factorySupplyReady(0,223) && factorySupplyReady(0,224));
    assert(!factorySupplyReady(8,256) && factorySupplyReady(0,256));
    for(uint32_t bits=0;bits<65536;++bits) {
        assert(((((bits&~0x771u)|work.coreWord)^bits)&~0x771u)==0);
        assert(((((bits&~0x71u)|work.systemWord)^bits)&~0x71u)==0);
    }
    puts("PASS: factory 384 MHz OPP, 700 mV minimum, SYS divider and unrelated clock bits");
}
