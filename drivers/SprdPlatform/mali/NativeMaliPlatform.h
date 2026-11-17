/* SPDX-License-Identifier: GPL-2.0-only */
/* The only way driver code reaches hardware.
 *
 * Every module that programs registers takes one of these instead of touching a
 * mapped pointer, so host tests can drive the real sequence against fake
 * registers and this file stays the single place that knows nothing.
 *
 * Offsets are relative to the window the caller mapped (SPM at 0x10006000, MFG
 * config at 0x13000000, the GPU at 0x13040000), not absolute addresses: a module
 * cannot accidentally program another block. IOError means the access itself
 * failed (no mapping, bus abort); anything else the target returns is passed
 * through unchanged.
 */
#ifndef NATIVE_MALI_PLATFORM_H
#define NATIVE_MALI_PLATFORM_H
#include <IOKit/IOTypes.h>
#include <stdint.h>

class NativeMaliRegisterTarget {
public:
    NativeMaliRegisterTarget() = default;
    virtual ~NativeMaliRegisterTarget() = default;
    NativeMaliRegisterTarget(const NativeMaliRegisterTarget &) = delete;
    NativeMaliRegisterTarget &operator=(const NativeMaliRegisterTarget &) = delete;
    virtual IOReturn read32(uint32_t offset, uint32_t *value) = 0;
    virtual IOReturn write32(uint32_t offset, uint32_t value) = 0;
    /* Sleep for at least `microseconds`. A caller polls with this; the real
     * implementation is a busy-wait or a timed wait, whichever XNU allows
     * (docs/OPEN_QUESTIONS.md item 5). */
    virtual IOReturn delay(uint64_t microseconds) = 0;
};
#endif
