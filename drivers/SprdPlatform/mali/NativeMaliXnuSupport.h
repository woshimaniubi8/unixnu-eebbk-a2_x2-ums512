/* SPDX-License-Identifier: GPL-2.0-only */
/* XNU-only adapters: the four injected interfaces implemented on IOKit.
 *
 * UNVERIFIED(XNU) - this whole file has never been compiled against XNU
 * headers (only -fsyntax-only against local stubs). This repository has no
 * kext build and the host tests do not include it. Every
 * IOKit call below appears in NativeAdreno's in-tree driver (MIT, see NOTICE)
 * with the same shape; the Mali-specific parts (window bases, span merging,
 * cache policy) are the ones docs/UNVERIFIED.md lists. Read that document
 * before trusting any line here.
 *
 * What lives here and what does not:
 *   - NativeMaliXnuWindow turns one mapped physical window into the
 *     NativeMaliRegisterTarget the pure layers already take. It knows nothing
 *     about which register is which.
 *   - NativeMaliXnuPages hands out 4 KiB page-table pages.
 *   - NativeMaliXnuBuffers is the NativeMaliMemorySource: one
 *     IOBufferMemoryDescriptor per buffer object, and the physical spans the
 *     address space maps. The descriptors are kept so the user client can hand
 *     the same object to clientMemoryForType().
 *   - NativeMaliXnuCache is the NativeMaliCacheMaintainer: the dc cvac/civac/
 *     ivac sequence, with the same whole-page rule as NativeAdrenoCpu.cpp.
 *
 * The adapters are not thread-safe. The user client serialises every call with
 * one IOLock; without that, two BO allocations would race for the same entry.
 */
#ifndef NATIVE_MALI_XNU_SUPPORT_H
#define NATIVE_MALI_XNU_SUPPORT_H
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>
#include "NativeMaliBoTable.h"
#include "NativeMaliCache.h"
#include "NativeMaliMemory.h"
#include "NativeMaliPlatform.h"
#include "NativeMaliVm.h"

/* One mapped register window. `base`/`bytes` are bound by the service while the
 * IOMemoryMap that owns the virtual range stays alive there; this object only
 * borrows the address. Offsets are relative to the window, as
 * NativeMaliPlatform.h requires. */
class NativeMaliXnuWindow final : public NativeMaliRegisterTarget {
public:
    NativeMaliXnuWindow() = default;
    void bind(volatile uint8_t *base, size_t bytes)
    {
        base_ = base;
        bytes_ = bytes;
    }
    bool bound() const { return base_ != nullptr && bytes_ != 0; }

    IOReturn read32(uint32_t offset, uint32_t *value) override;
    IOReturn write32(uint32_t offset, uint32_t value) override;
    IOReturn delay(uint64_t microseconds) override;

private:
    volatile uint8_t *base_ = nullptr;
    size_t bytes_ = 0;
};

/* Page-table storage. `limit` is the output-address ceiling (1 << oas) the same
 * buffer source uses; a page outside it is refused, not wrapped. */
class NativeMaliXnuPages final : public NativeMaliPageSource {
public:
    NativeMaliXnuPages() = default;
    void setOutputLimit(uint32_t oas_bits);

    IOReturn alloc(NativeMaliPage *page) override;
    void release(const NativeMaliPage &page) override;
    size_t live() const;

private:
    struct Entry {
        void *cpu = nullptr;
        IOBufferMemoryDescriptor *descriptor = nullptr;
    };
    Entry entry_[NativeMaliVmMaxTables]{};
    uint64_t limit_ = UINT64_C(1) << 40;
};

/* The buffer objects' storage. One entry per live NativeMaliMemory; release()
 * finds it by the CPU address the caller was given. */
class NativeMaliXnuBuffers final : public NativeMaliMemorySource {
public:
    NativeMaliXnuBuffers() = default;
    void setOutputLimit(uint32_t oas_bits);

    IOReturn alloc(size_t bytes, void **cpu, NativeMaliMemorySpan *spans,
        size_t *count) override;
    void release(void *cpu, const NativeMaliMemorySpan *spans,
        size_t count) override;

    /* The descriptor behind one live buffer, retained for the caller (the user
     * client's clientMemoryForType()). Null for an address this source did not
     * hand out. */
    IOMemoryDescriptor *descriptor(void *cpu);

private:
    struct Entry {
        void *cpu = nullptr;
        IOBufferMemoryDescriptor *descriptor = nullptr;
        size_t bytes = 0;
    };
    Entry entry_[NativeMaliBoTableCapacity]{};
    uint64_t limit_ = UINT64_C(1) << 40;
};

/* dc cvac / dc civac / dc ivac over whole pages, preceded and followed by a
 * dsb sy, with preemption disabled while CTR_EL0 and the operations are read.
 * That is the sequence NativeAdrenoCpu.cpp uses on the same kernel; it has not
 * been run on this phone. */
class NativeMaliXnuCache final : public NativeMaliCacheMaintainer {
public:
    IOReturn maintain(void *cpu, size_t bytes,
        NativeMaliCacheOperation operation) override;
};
#endif
