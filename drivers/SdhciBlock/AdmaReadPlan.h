// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef BBK_ADMA_READ_PLAN_H
#define BBK_ADMA_READ_PLAN_H
#include <stdint.h>
#include <stddef.h>

// Preserve the hardware-qualified 16-byte, 64-bit-address descriptor format.
// Standard SDHCI scatter/gather chaining is also used by Linux's sdhci.c.
// Keep each positive length at 32 KiB: no dependence on zero meaning 64 KiB.
constexpr uint32_t kBbkAdmaSegmentBytes = 32768;
constexpr uint32_t kBbkAdmaMaximumBlocks = 512;
constexpr uint32_t kBbkAdmaBlockBytes = 512;
constexpr size_t kBbkAdmaWordsPerDescriptor = 4;
constexpr size_t kBbkAdmaMaximumDescriptors =
    kBbkAdmaMaximumBlocks * kBbkAdmaBlockBytes / kBbkAdmaSegmentBytes;

static inline size_t
bbkBuildAdmaReadPlan(uint32_t *words, size_t capacityWords, uint64_t physical,
                     uint32_t blocks)
{
    if (!words || !blocks || blocks > kBbkAdmaMaximumBlocks || (physical & 3)) return 0;
    const uint32_t bytes = blocks * kBbkAdmaBlockBytes;
    const size_t entries = (bytes + kBbkAdmaSegmentBytes - 1) / kBbkAdmaSegmentBytes;
    if (entries > capacityWords / kBbkAdmaWordsPerDescriptor ||
        physical > UINT64_MAX - (bytes - 1)) return 0;
    uint32_t remaining = bytes;
    for (size_t i = 0; i < entries; ++i) {
        const uint32_t length = remaining < kBbkAdmaSegmentBytes ? remaining : kBbkAdmaSegmentBytes;
        // Valid | Transfer, with End exclusively on the final transfer entry.
        words[i * 4] = (length << 16) | (i + 1 == entries ? 0x23U : 0x21U);
        words[i * 4 + 1] = static_cast<uint32_t>(physical);
        words[i * 4 + 2] = static_cast<uint32_t>(physical >> 32);
        words[i * 4 + 3] = 0;
        physical += length;
        remaining -= length;
    }
    return entries;
}
#endif
