// SPDX-License-Identifier: MIT
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "../../drivers/SprdPlatform/mali/SprdMaliJobRouting.h"

using SprdMaliJobRouting::Type;

static void expect(uint32_t present, Type type, bool valid, uint32_t expected)
{
    uint32_t actual = 0xfeed;
    const bool result = SprdMaliJobRouting::choose(present, type, &actual);
    assert(result == valid);
    if(valid)assert(actual == expected);
}

int main()
{
    for(const Type type : {Type::WriteValue, Type::CacheFlush, Type::Vertex,
                           Type::Geometry, Type::Tiler, Type::Fused,
                           Type::IndexedVertex})
        expect(0x7, type, true, 1);
    expect(0x7, Type::Fragment, true, 0);
    expect(0x7, Type::Compute, true, 2);

    expect(0x6, Type::Fragment, false, 0);
    expect(0x5, Type::Vertex, false, 0);
    expect(0x3, Type::Compute, false, 0);
    expect(0x7, static_cast<Type>(0xff), false, 0);
    assert(!SprdMaliJobRouting::choose(0x7, Type::Fragment, nullptr));
    std::puts("PASS: Bifrost JS0 fragment, JS1 geometry/tiler, JS2 compute routing");
    return 0;
}
