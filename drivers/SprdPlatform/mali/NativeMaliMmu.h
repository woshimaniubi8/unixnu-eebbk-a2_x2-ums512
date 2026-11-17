/* SPDX-License-Identifier: GPL-2.0-only */
/* The GPU's own MMU: AS slot allocation and the enable/disable sequence.
 *
 * Source (content-pinned, third_party/PINNED.md):
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:72-89
 *     wait_ready(): poll AS_STATUS until AS_ACTIVE is clear, every 10 us for
 *     100000 us.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:91-100
 *     write_cmd(): wait_ready() first, then write AS_COMMAND.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:103-137
 *     lock_region(): the locked region is a naturally aligned power-of-two
 *     block encoded as log2 minus one, at least AS_LOCK_REGION_MIN_SIZE
 *     (32 KiB); the width is the highest differing bit of start and end - 1,
 *     and start is rounded down to that block. A zero size writes nothing.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:139-152
 *     mmu_hw_do_operation_locked(): lock the region (unless the command is
 *     UNLOCK), issue the command, then wait_ready().
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:167-190
 *     panfrost_mmu_enable(): FLUSH_MEM over the whole space, write TRANSTAB,
 *     MEMATTR and TRANSCFG as LO/HI pairs, then UPDATE.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:191-205
 *     panfrost_mmu_disable(): FLUSH_MEM, zero TRANSTAB and MEMATTR, set
 *     TRANSCFG to ADRMODE_UNMAPPED, then UPDATE.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:207-217
 *     the ARM_MALI_LPAE configuration uses TRANSCFG = ADRMODE_LEGACY (0) and
 *     takes TRANSTAB/MEMATTR from io-pgtable-arm's arm_mali_lpae_cfg.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:262-330
 *     panfrost_mmu_as_get(): an address space is the first free bit of
 *     as_alloc_mask, and the bit must be set in features.as_present. The LRU
 *     reclaim of a busy slot is not implemented here: this class refuses
 *     instead, because there are no jobs yet.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:892-905
 *     panfrost_mmu_irq_handler_thread() reads AS_FAULTSTATUS and
 *     AS_FAULTADDRESS_LO/HI; readFault() does the same and hands the words to
 *     NativeMaliFault. It does not acknowledge the interrupt.
 *   drivers/gpu/drm/panfrost/panfrost_mmu.c:389-404
 *     panfrost_mmu_flush_range(): lock the updated IOVA range and issue
 *     FLUSH_PT, then wait; this is what a mapping or unmapping needs.
 *   drivers/gpu/drm/panfrost/panfrost_regs.h
 *     MMU_BASE, MMU_AS(), AS_* and AS_COMMAND_*; generated into
 *     NativeMaliRegs.h by tools/generate-regs.sh.
 *
 * The MMU registers live at 0x2400 inside the same 0x13040000 window as the GPU
 * core, so they are only safe to touch after NativeMaliPower::gpuReadable().
 * enable() and disable() re-check that gate on every call.
 *
 * The MMU interrupt registers are deliberately untouched: panfrost unmasks
 * them in panfrost_mmu_init(), but this driver has no interrupt handler yet,
 * and an unmasked fault with nothing to clear it would storm. MMU_INT_CLEAR /
 * MMU_INT_MASK belong to NativeMaliIrq.
 *
 * AS_COMMAND_UNLOCK is not used: no pinned caller issues it (the only check is
 * in mmu_hw_do_operation_locked(), panfrost_mmu.c:145), so inventing a
 * standalone unlock would not match the source. Mapping updates use
 * flushRange(), which is the FLUSH_PT path panfrost actually takes.
 *
 * Writes are behind NATIVE_MALI_M4_WRITES (default 0). Allocation is pure
 * bookkeeping and works without the switch; the driver still needs the on-device
 * probe accepted before anyone enables writes.
 */
#ifndef NATIVE_MALI_MMU_H
#define NATIVE_MALI_MMU_H
#include "NativeMaliFault.h"
#include "NativeMaliPlatform.h"
#include "NativeMaliPower.h"
#include "NativeMaliVm.h"
#include <IOKit/IOTypes.h>
#include <stdint.h>

#ifndef NATIVE_MALI_M4_WRITES
#define NATIVE_MALI_M4_WRITES 0
#endif

enum : uint32_t { NativeMaliMmuMaxAs = 32 };

class NativeMaliMmu final {
public:
    enum : uint64_t {
        DefaultReadyTimeoutUs = 100000,
        ReadyPollUs = 10
    };

    /* False unless the build opts in; the driver build never sets it. */
    static constexpr bool kWriteAuthorized = NATIVE_MALI_M4_WRITES != 0;

    NativeMaliMmu() = default;
    ~NativeMaliMmu() = default;
    NativeMaliMmu(const NativeMaliMmu &) = delete;
    NativeMaliMmu &operator=(const NativeMaliMmu &) = delete;

    /* `registers` is the whole 0x13040000 page (the MMU sits at 0x2400).
     * `power` must already be prepared. Re-preparing with the same pair is
     * allowed; a different pair is refused so a live AS is never moved to
     * another register window. */
    IOReturn prepare(NativeMaliRegisterTarget *registers,
        NativeMaliPower *power);
    bool prepared() const { return registers_ && power_ && power_->prepared(); }

    /* Record GPU_AS_PRESENT from NativeMaliGpuCore::probe(). Refuses an empty
     * mask; a different mask after the first call is refused, because the
     * slots already handed out would no longer mean the same space. */
    IOReturn setPresent(uint32_t asPresent);
    uint32_t present() const { return asPresent_; }

    /* First free slot that exists in the present mask, lowest bit first as
     * ffz(as_alloc_mask) does. kIOReturnNoResources when every present slot is
     * in use. Requires prepare() and setPresent(). */
    IOReturn allocate(uint32_t *as);
    /* Give a slot back. It must have been allocated. This does not touch the
     * hardware: disable() first if the AS is still programmed. */
    IOReturn release(uint32_t as);
    /* Take a specific slot back after resetAllocation() cleared the bitmap.
     * The owner of an address space keeps its number across a GPU reset: the
     * hardware forgot the TRANSTAB it was programmed with, but the number was
     * never free for anybody else, so the service puts the same bit back and
     * enables it again. Refuses an absent or out-of-range slot, and one that
     * is already allocated (that would be two owners for one number). Pure
     * bookkeeping, no register access. */
    IOReturn reclaim(uint32_t as);
    bool isAllocated(uint32_t as) const;
    uint32_t allocatedMask() const { return allocated_; }

    /* panfrost_mmu_reset() bookkeeping (panfrost_mmu.c:345-365): forget which
     * slots are allocated and which the driver marked faulty, because a GPU
     * reset tore down whatever the hardware had programmed. The source also
     * rearms the MMU interrupts there; this driver keeps them masked until
     * NativeMaliIrq exists (docs/TODO.md, M3). The AS fault mask has no
     * counterpart here yet, so only the allocation bitmap is cleared.
     * present() survives: GPU_AS_PRESENT is a hardware fact, not an allocation.
     * panfrost calls this after panfrost_gpu_power_on() in
     * panfrost_device_reset() (panfrost_device.c:517). No register access,
     * so it works without NATIVE_MALI_M4_WRITES. */
    void resetAllocation();

    /* panfrost_mmu_enable() for the ARM_MALI_LPAE format: the whole-space
     * FLUSH_MEM (which locks the region first), TRANSTAB/MEMATTR from `vm`,
     * TRANSCFG = ADRMODE_LEGACY, then UPDATE. `as` must be present, allocated
     * and the VM prepared. On a configuration-write failure the AS is disabled
     * best effort and the original error is returned. */
    IOReturn enable(uint32_t as, const NativeMaliVm &vm,
        uint64_t timeout_us = DefaultReadyTimeoutUs);
    /* panfrost_mmu_disable(): FLUSH_MEM, zero TRANSTAB/MEMATTR, TRANSCFG =
     * ADRMODE_UNMAPPED, UPDATE. Leaves the slot allocated. */
    IOReturn disable(uint32_t as,
        uint64_t timeout_us = DefaultReadyTimeoutUs);

    /* panfrost_mmu_flush_range() (panfrost_mmu.c:389-404): lock [start,
     * start + size) and issue FLUSH_PT, then wait for AS_ACTIVE to clear. This
     * is the step a mapping change needs after NativeMaliVm updates the
     * tables; the caller still has to publish the page-table memory itself
     * (docs/OPEN_QUESTIONS.md item 5). A zero size is refused: lock_region()
     * writes nothing for it, so it would be a flush of nothing. The slot must
     * be present and allocated, because enabling the AS is what programs it. */
    IOReturn flushRange(uint32_t as, uint64_t start, uint64_t size,
        uint64_t timeout_us = DefaultReadyTimeoutUs);

    /* panfrost_mmu_irq_handler_thread() (panfrost_mmu.c:892-905): read
     * AS_FAULTSTATUS and AS_FAULTADDRESS_LO/HI for one address space and decode
     * them with NativeMaliFault. Read-only. The slot must be present on this
     * GPU, because the window for a missing AS is not safe to touch. The MMU
     * interrupt registers stay untouched: this does not acknowledge anything,
     * it only reports what the AS latched. */
    IOReturn readFault(uint32_t as, NativeMaliMmuFault *fault);

    /* The AS_LOCKADDR value lock_region() computes for [start, start + size),
     * or false when size is zero (the source writes nothing) or the range
     * wraps. The width is log2(block) - 1; the base is start rounded down to
     * that block. */
    static bool lockRegionValue(uint64_t start, uint64_t size,
        uint64_t *value);

private:
    IOReturn waitReady(uint32_t as, uint64_t timeout_us);
    IOReturn writeCommand(uint32_t as, uint32_t command,
        uint64_t timeout_us);
    IOReturn operate(uint32_t as, uint64_t start, uint64_t size,
        uint32_t command, uint64_t timeout_us);
    IOReturn writePair(uint32_t low, uint32_t high, uint64_t value);

    NativeMaliRegisterTarget *registers_ = nullptr;
    NativeMaliPower *power_ = nullptr;
    uint32_t asPresent_ = 0;
    uint32_t allocated_ = 0;
};
#endif
