/* SPDX-License-Identifier: GPL-2.0-only */
/* What the GPU says it is, decoded exactly as panfrost does it.
 *
 * Source (content-pinned, third_party/PINNED.md):
 *   drivers/gpu/drm/panfrost/panfrost_gpu.c, panfrost_gpu_init() lines ~316-345
 *       revision = GPU_ID & 0xffff; id = GPU_ID >> 16
 *       the T60x oddball id 0x6956 is rewritten to 0x0600
 *       major = (revision >> 12) & 0xf, minor = (revision >> 4) & 0xff,
 *       status = revision & 0xf
 *       the model table below is its gpu_models[] entries, Bifrost only
 *   drivers/gpu/drm/panfrost/panfrost_device.h, panfrost_model_cmp() /
 *       panfrost_model_is_bifrost()
 *       a product id with bit 12..15 set is compared after masking with 0xf00f,
 *       and anything at or above 0x1000 is Bifrost
 *
 * This only maps an already-read GPU_ID register value to a model name and its
 * feature facts; it reads no register. Reading GPU_ID is M3 and needs the MFG
 * power domains up (M2), because reading 0x13040000 while the domain is off can
 * hang the bus.
 *
 * Why refuse an unknown id instead of falling back: panfrost fails the probe
 * ("GPU model not found"), and it is right to. Feature and issue bits are chosen
 * by model and revision, so guessing them means writing the wrong JM, L2 and MMU
 * configuration. If this board turns out to report a MediaTek-specific id (the
 * source notes MT8192's G57 does), the id is added here only with the value read
 * from this device, never by analogy.
 */
#ifndef NATIVE_MALI_GPU_H
#define NATIVE_MALI_GPU_H
#include <IOKit/IOTypes.h>
#include <stdint.h>

struct NativeMaliGpuIdentity {
    uint16_t product;    /* GPU_ID >> 16, after the T60x fixup */
    uint16_t revision;   /* GPU_ID & 0xffff */
    uint16_t matched;    /* the table id it matched, 0 when none */
    uint8_t major;       /* (revision >> 12) & 0xf */
    uint8_t minor;       /* (revision >> 4) & 0xff */
    uint8_t status;      /* revision & 0xf */
    uint8_t bifrost;     /* panfrost_model_is_bifrost() */
    /* Whether a per-revision entry in the source's table matched. panfrost uses
     * it only to add that revision's extra issue bits; a model whose revision
     * has no entry still probes, so this is not a validity flag. */
    uint8_t revision_specific;
    const char *model;   /* "g52" and so on, null when unknown */
};

class NativeMaliGpu final {
    NativeMaliGpu() = delete;
public:
    /* The product-id comparison panfrost_model_cmp() performs. */
    static uint16_t maskProduct(uint16_t product);

    /* Decode one GPU_ID value. Fills `out` on both success and refusal, so a
     * caller can log what the hardware claimed; refuses with
     * kIOReturnUnsupported for a product id this driver has no pinned facts for
     * (that includes every Midgard). kIOReturnBadArgument for a null output. */
    static IOReturn identify(uint32_t gpu_id, NativeMaliGpuIdentity *out);
};
#endif
