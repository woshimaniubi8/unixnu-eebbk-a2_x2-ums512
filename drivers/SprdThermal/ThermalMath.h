// SPDX-License-Identifier: GPL-2.0-only
// Numeric conversion from Spreadtrum's sprd_thermal.c (sharkl5_data).
#pragma once
#include <stdint.h>

namespace SprdThermalMath {
struct Calibration { int32_t slope; int32_t offset; };
inline bool calibrate(uint32_t sign, uint32_t ratioOffset, uint32_t delta,
                      Calibration &out)
{
    if (sign > 1 || ratioOffset > 127 || delta > 255) return false;
    int32_t ratio = sign ? 1000 - (int32_t)ratioOffset : 1000 + (int32_t)ratioOffset;
    out.slope = (262 * ratio) / 1000;
    out.offset = 66400 + ((int32_t)delta - 128) * 250;
    return out.slope > 0;
}
inline bool convert(const Calibration &cal, uint32_t raw, int32_t &milliC)
{
    if (raw > 1000 || cal.slope <= 0) return false;
    milliC = cal.slope * (int32_t)raw - cal.offset;
    // Preserve over-temperature readings so the critical path can act on them.
    return milliC >= -40000 && milliC <= 300000;
}
inline int32_t fixedC(int32_t milliC)
{
    return (int32_t)(((int64_t)milliC * 65536) / 1000);
}
inline uint32_t threshold(const Calibration &cal, int32_t milliC)
{
    int32_t raw = (milliC + cal.offset) / cal.slope;
    return raw < 0 ? 0u : raw > 999 ? 999u : (uint32_t)raw;
}
}
