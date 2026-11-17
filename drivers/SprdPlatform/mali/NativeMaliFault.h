/* SPDX-License-Identifier: GPL-2.0-only */
/* Decoding for the two fault records: the GPU's own (job) fault and the MMU
 * address-space fault.
 *
 * Source (content-pinned, third_party/PINNED.md):
 *   drivers/gpu/drm/panfrost/panfrost_gpu.c:27-43
 *     the GPU interrupt handler reads GPU_FAULT_STATUS and the
 *     GPU_FAULT_ADDRESS_HI/LO pair, and prints the low byte of the status as an
 *     exception name.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:892-905
 *     the MMU handler reads AS_FAULTSTATUS and AS_FAULTADDRESS_LO/HI, then
 *     decodes exception = status & 0xFF, access = (status >> 8) & 0x3 and
 *     source id = status >> 16.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:908-928
 *     "page fault only" means (exception & 0xF8) == 0xC0, and bit 10 of the
 *     status distinguishes a decoder fault from a slave fault.
 *   drivers/gpu/drm/panfrost/panfrost_device.h:258-330
 *     the exception code values.
 *   drivers/gpu/drm/panfrost/panfrost_device.c:418-500
 *     the name table; a code that is absent from it (or >= its size, 0xF0)
 *     prints "Unknown exception type".
 *   drivers/gpu/drm/panfrost/panfrost_regs.h
 *     AS_FAULTSTATUS_ACCESS_TYPE_*; generated into NativeMaliRegs.h.
 *
 * Everything here is pure: no IO and no allocation, so the encoding is tested
 * without hardware. The register reads live in NativeMaliGpuCore::readFault()
 * and NativeMaliMmu::readFault().
 */
#ifndef NATIVE_MALI_FAULT_H
#define NATIVE_MALI_FAULT_H
#include <stdint.h>

/* No fault code above 0x3F is informational; everything higher is a fault.
 * panfrost_exception_is_fault() (panfrost_device.h:331-335) is exactly this
 * comparison against DRM_PANFROST_EXCEPTION_MAX_NON_FAULT. */
enum : uint32_t { NativeMaliFaultMaxNonFault = 0x3F };

struct NativeMaliGpuFault {
    uint32_t status = 0;      /* GPU_FAULT_STATUS */
    uint32_t addressLo = 0;   /* GPU_FAULT_ADDRESS_LO */
    uint32_t addressHi = 0;   /* GPU_FAULT_ADDRESS_HI */

    uint32_t exception() const { return status & 0xFF; }
    uint64_t address() const
    {
        return (static_cast<uint64_t>(addressHi) << 32) | addressLo;
    }
    bool isFault() const { return exception() > NativeMaliFaultMaxNonFault; }
};

struct NativeMaliMmuFault {
    uint32_t status = 0;      /* AS_FAULTSTATUS */
    uint32_t addressLo = 0;   /* AS_FAULTADDRESS_LO */
    uint32_t addressHi = 0;   /* AS_FAULTADDRESS_HI */

    uint32_t exception() const { return status & 0xFF; }
    /* AS_FAULTSTATUS_ACCESS_TYPE_* (panfrost_regs.h). */
    uint32_t access() const { return (status >> 8) & 0x3; }
    uint32_t sourceId() const { return status >> 16; }
    uint64_t address() const
    {
        return (static_cast<uint64_t>(addressHi) << 32) | addressLo;
    }
    bool decoderFault() const { return (status & (1u << 10)) != 0; }
    /* The only case the source tries to resolve by mapping a page; every other
     * fault is terminal (panfrost_mmu.c:908-914). */
    bool pageFault() const { return (exception() & 0xF8) == 0xC0; }
    bool isFault() const { return exception() > NativeMaliFaultMaxNonFault; }
};

class NativeMaliFault final {
public:
    NativeMaliFault() = delete;

    /* panfrost_exception_name(): the pinned name, or "Unknown exception type"
     * for a code the table does not name. */
    static const char *exceptionName(uint32_t exceptionCode);

    /* access_type_name(). G52 has HW_FEATURE_AARCH64_MMU
     * (panfrost_features.h:73-83), so access type 0 is ATOMIC; without that
     * feature panfrost reports it as UNKNOWN. */
    static const char *accessName(uint32_t accessType);
};
#endif
