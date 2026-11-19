// SPDX-License-Identifier: GPL-2.0-only
// Fixed, board-sized off-screen targets; no live framebuffer or user pointers.
#pragma once
#include "SprdMaliRender.h"

namespace SprdMaliFullFrame {
constexpr unsigned Width=1200, Height=1920, Stride=4864;
constexpr unsigned PitchWords=Stride/sizeof(uint32_t);
constexpr size_t HeapBytes=0x20000, HeapEnd=SprdMaliRender::HeapOffset+HeapBytes;
constexpr size_t ImageOffset=0x28000, ImageBytes=Stride*Height;
constexpr size_t BufferBytes=ImageOffset+ImageBytes+0x4000;
constexpr uint32_t Background=0xffb3812d, Accent=0xff35cc92, Sentinel=0xa5a5a5a5;
constexpr unsigned AccentX=704, AccentY=1136;
constexpr unsigned Passes=2;

static_assert(Stride%64==0 && Stride>Width*4);
static_assert((Width|Height|AccentX|AccentY)%16==0);
static_assert(ImageOffset>HeapEnd && (ImageOffset|ImageBytes|BufferBytes)%0x4000==0);
// Mesa v7 counts 8-byte bin pointers, rounded to groups of eight. Reserve
// space for all 16x16 bins, with extra space in the heap for hardware state.
static_assert(((Width/16)*(Height/16)+7)/8*64 < HeapBytes);

inline bool build(void *cpu, size_t bytes, uint64_t gpu, unsigned pass)
{
    if(!cpu || bytes!=BufferBytes || (reinterpret_cast<uintptr_t>(cpu)&63) ||
        !gpu || (gpu&4095) || gpu>UINT64_MAX-bytes || pass>=Passes)return false;
    auto *p=static_cast<uint8_t *>(cpu);
    memset(p,0,HeapEnd);
    constexpr unsigned maxX=Width-1, maxY=Height-1;
    const unsigned minX=pass?AccentX:0, minY=pass?AccentY:0;
    const uint32_t color=pass?Accent:Background;
    using namespace SprdMaliMesa;
    using namespace SprdMaliRender;

    MALI_JOB_HEADER job={};
    job.is_64b=true;job.type=MALI_JOB_TYPE_FRAGMENT;job.index=1;
    MALI_JOB_HEADER_pack(reinterpret_cast<mali_job_header_packed *>(p+JobOffset),&job);
    MALI_FRAGMENT_JOB_PAYLOAD payload={};
    payload.bound_min_x=minX>>4;payload.bound_min_y=minY>>4;
    payload.bound_max_x=maxX>>4;payload.bound_max_y=maxY>>4;
    payload.framebuffer=(gpu+FramebufferOffset)|1;
    MALI_FRAGMENT_JOB_PAYLOAD_pack(reinterpret_cast<mali_fragment_job_payload_packed *>(p+32),&payload);

    MALI_FRAMEBUFFER_PARAMETERS fb={};
    fb.width=Width;fb.height=Height;fb.bound_min_x=minX;fb.bound_min_y=minY;
    fb.bound_max_x=maxX;fb.bound_max_y=maxY;fb.sample_count=1;
    fb.effective_tile_size=256;fb.tie_break_rule=MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
    fb.render_target_count=1;fb.color_buffer_allocation=1024;
    fb.z_internal_format=MALI_Z_INTERNAL_FORMAT_D24;
    fb.tiler=gpu+TilerOffset;fb.sample_locations=gpu+SampleOffset;
    MALI_FRAMEBUFFER_PARAMETERS_pack(reinterpret_cast<mali_framebuffer_parameters_packed *>(p+FramebufferOffset),&fb);

    MALI_RGB_RENDER_TARGET rt={};
    rt.internal_format=MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
    rt.write_enable=true;rt.writeback_format=MALI_COLOR_FORMAT_R8G8B8A8;
    rt.writeback_block_format=MALI_BLOCK_FORMAT_LINEAR;rt.writeback_msaa=MALI_MSAA_SINGLE;
    rt.swizzle=0x688;
    rt.clean_tile_write_enable=true;
    rt.writeback_buffer.base=gpu+ImageOffset;
    rt.writeback_buffer.row_stride=Stride;rt.writeback_buffer.surface_stride=ImageBytes;
    rt.clear.color_0=rt.clear.color_1=rt.clear.color_2=rt.clear.color_3=color;
    MALI_RGB_RENDER_TARGET_pack(reinterpret_cast<mali_rgb_render_target_packed *>(p+FramebufferOffset+128),&rt);

    MALI_TILER_HEAP heap={};
    heap.size=HeapBytes;heap.base=heap.bottom=gpu+HeapOffset;heap.top=gpu+HeapEnd;
    MALI_TILER_HEAP_pack(reinterpret_cast<mali_tiler_heap_packed *>(p+HeapDescOffset),&heap);
    MALI_TILER_CONTEXT tiler={};
    tiler.hierarchy_mask=1;tiler.fb_width=Width;tiler.fb_height=Height;
    tiler.heap=gpu+HeapDescOffset;
    MALI_TILER_CONTEXT_pack(reinterpret_cast<mali_tiler_context_packed *>(p+TilerOffset),&tiler);
    auto *samples=reinterpret_cast<uint16_t *>(p+SampleOffset);
    samples[0]=samples[1]=128;samples[64]=samples[65]=128;
    return true;
}

struct Check {
    unsigned pixels=0, padding=0, guards=0;
};

inline bool verify(const void *cpu, size_t bytes, unsigned pass, Check &check)
{
    check={};
    if(!cpu || bytes!=BufferBytes || (reinterpret_cast<uintptr_t>(cpu)&3) ||
        pass>=Passes)return false;
    const auto *words=static_cast<const volatile uint32_t *>(cpu);
    const auto *image=words+ImageOffset/sizeof(uint32_t);
    for(unsigned y=0;y<Height;++y) {
        const auto *row=image+y*PitchWords;
        for(unsigned x=0;x<Width;++x)
            check.pixels+=row[x]!=(pass && x>=AccentX && y>=AccentY?Accent:Background);
        for(unsigned x=Width;x<PitchWords;++x)check.padding+=row[x]!=Sentinel;
    }
    for(size_t i=HeapEnd/4;i<ImageOffset/4;++i)check.guards+=words[i]!=Sentinel;
    for(size_t i=(ImageOffset+ImageBytes)/4;i<BufferBytes/4;++i)
        check.guards+=words[i]!=Sentinel;
    return !check.pixels && !check.padding && !check.guards;
}
}
