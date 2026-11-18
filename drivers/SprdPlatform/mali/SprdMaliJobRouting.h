// SPDX-License-Identifier: MIT
// Bifrost Job Manager routing derived from Mesa panfrost_job_get_slot().
// Keep this policy independent of IOKit so it can be exhaustively host-tested.
#pragma once
#include <stdint.h>

namespace SprdMaliJobRouting {

enum class Type : uint32_t {
    WriteValue = 2,
    CacheFlush = 3,
    Compute = 4,
    Vertex = 5,
    Geometry = 6,
    Tiler = 7,
    Fused = 8,
    Fragment = 9,
    IndexedVertex = 10
};

/*
 * Mesa's G52 scheduler reserves JS0 for fragment work, JS1 for geometry and
 * tiler work, and JS2 for compute. Write-value/cache-flush jobs are setup for
 * the geometry path and therefore use JS1. An absent required slot is a hard
 * error; silently moving a job to another slot changes its hardware contract.
 */
inline bool choose(uint32_t js_present, Type type, uint32_t *slot)
{
    if(!slot)return false;
    uint32_t selected = 0;
    switch(type) {
    case Type::Fragment:
        selected = 0;
        break;
    case Type::Compute:
        selected = 2;
        break;
    case Type::WriteValue:
    case Type::CacheFlush:
    case Type::Vertex:
    case Type::Geometry:
    case Type::Tiler:
    case Type::Fused:
    case Type::IndexedVertex:
        selected = 1;
        break;
    default:
        return false;
    }
    if((js_present & (1u << selected)) == 0)return false;
    *slot = selected;
    return true;
}

} // namespace SprdMaliJobRouting
