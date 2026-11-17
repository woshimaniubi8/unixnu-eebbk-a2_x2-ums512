/* SPDX-License-Identifier: GPL-2.0-only */
/* Model table and revision decode; see NativeMaliGpu.h for the source of every
 * number. No register is touched here. */
#include "NativeMaliGpu.h"

namespace {

/* panfrost_gpu.c gpu_models[], Bifrost entries only. `revision` is the entry a
 * GPU_REV(model, r, p) line names — (r << 12) | (p << 4) | status — or 0xffff
 * for a bare GPU_MODEL(model, id) with no per-revision issues. A revision that
 * does not appear here still matches the model: the source only uses the entry
 * to add extra issue bits. */
struct Model {
    uint16_t id;
    const char *name;
    uint16_t revision;
};

constexpr Model kModels[] = {
    {0x6000, "g71", 0x0001},   /* GPU_REV_EXT(g71, 0, 0, 1, _05dev0) */
    {0x6001, "g72", 0xffff},
    {0x7000, "g51", 0xffff},
    {0x7001, "g76", 0xffff},
    {0x7002, "g52", 0xffff},
    {0x7003, "g31", 0x1000},   /* GPU_REV(g31, 1, 0) */
    {0x9001, "g57", 0x0000},   /* GPU_REV(g57, 0, 0) */
};

enum : uint16_t { AnyRevision = 0xffff, T60xId = 0x6956, T60xFixedId = 0x0600 };

} // namespace

uint16_t NativeMaliGpu::maskProduct(uint16_t product)
{
    return (product & 0xf000) ? uint16_t(product & 0xf00f) : product;
}

IOReturn NativeMaliGpu::identify(uint32_t gpu_id, NativeMaliGpuIdentity *out)
{
    if(!out)return kIOReturnBadArgument;
    NativeMaliGpuIdentity id{};
    id.revision = uint16_t(gpu_id & 0xffff);
    id.product = uint16_t(gpu_id >> 16);
    /* The T60x has an oddball ID value; the source fixes it up to the standard
     * Midgard format. It is not a Bifrost and will be refused below, but the
     * fixup is kept so the decoded product is the one panfrost would report. */
    if(id.product == T60xId)id.product = T60xFixedId;
    id.major = uint8_t((id.revision >> 12) & 0xf);
    id.minor = uint8_t((id.revision >> 4) & 0xff);
    id.status = uint8_t(id.revision & 0xf);
    id.bifrost = maskProduct(id.product) >= 0x1000 ? 1 : 0;

    const uint16_t masked = maskProduct(id.product);
    *out = id;
    for(const Model &model : kModels) {
        if(maskProduct(model.id) != masked)continue;
        out->matched = model.id;
        out->model = model.name;
        out->revision_specific = (model.revision != AnyRevision &&
            model.revision == id.revision) ? 1 : 0;
        return kIOReturnSuccess;
    }
    /* Midgard, an unknown Bifrost, or a vendor-specific id: no pinned facts, so
     * no guess. The caller logs product and revision. */
    return kIOReturnUnsupported;
}
