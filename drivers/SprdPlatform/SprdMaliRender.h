// SPDX-License-Identifier: GPL-2.0-only
// Bounded Bifrost v7 fragment clear, using Mesa's generated packet packers.
// Packet/source provenance: hardware/mesa-bifrost/provenance.json.
#pragma once
#include "mali/MesaV7RenderPackets.h"

namespace SprdMaliRender {
constexpr size_t BufferBytes=0x10000,ImageOffset=0x8000,ImageBytes=0x4000;
constexpr unsigned Width=64,Height=64,Stride=Width*4;
constexpr uint64_t JobOffset=0,FramebufferOffset=0x100,HeapDescOffset=0x200;
constexpr uint64_t TilerOffset=0x240,SampleOffset=0x400,PolygonOffset=0x1000,HeapOffset=0x2000;
constexpr uint32_t Colors[4]={0xff0000ff,0xff00ff00,0xffff0000,0xffffffff};

inline bool build(void *cpu,size_t bytes,uint64_t gpu,unsigned quadrant) {
    if(!cpu || bytes!=BufferBytes || ((uintptr_t)cpu&63) || !gpu || (gpu&4095) ||
       gpu+bytes<gpu || quadrant>=4)return false;
    auto *p=(uint8_t *)cpu;
    // Only descriptors/empty tiler storage are CPU-written. Pixels stay in a
    // disjoint area initialized to a sentinel by the caller before submission.
    memset(p,0,0x4000);
    const unsigned minX=(quadrant&1)?32:0,minY=(quadrant&2)?32:0;
    const unsigned maxX=minX+31,maxY=minY+31;
    using namespace SprdMaliMesa;
    MALI_JOB_HEADER job={};job.is_64b=true;job.type=MALI_JOB_TYPE_FRAGMENT;job.index=1;
    MALI_JOB_HEADER_pack((mali_job_header_packed *)(p+JobOffset),&job);
    MALI_FRAGMENT_JOB_PAYLOAD payload={};
    payload.bound_min_x=minX>>4;payload.bound_min_y=minY>>4;
    payload.bound_max_x=maxX>>4;payload.bound_max_y=maxY>>4;
    payload.framebuffer=(gpu+FramebufferOffset)|1;
    MALI_FRAGMENT_JOB_PAYLOAD_pack((mali_fragment_job_payload_packed *)(p+32),&payload);
    MALI_FRAMEBUFFER_PARAMETERS fb={};
    fb.width=Width;fb.height=Height;fb.bound_min_x=minX;fb.bound_min_y=minY;
    fb.bound_max_x=maxX;fb.bound_max_y=maxY;fb.sample_count=1;
    fb.effective_tile_size=256;fb.tie_break_rule=MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
    fb.render_target_count=1;fb.color_buffer_allocation=1024;
    fb.z_internal_format=MALI_Z_INTERNAL_FORMAT_D24;
    fb.tiler=gpu+TilerOffset;fb.sample_locations=gpu+SampleOffset;
    MALI_FRAMEBUFFER_PARAMETERS_pack((mali_framebuffer_parameters_packed *)(p+FramebufferOffset),&fb);
    MALI_RGB_RENDER_TARGET rt={};
    rt.internal_format=MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
    rt.write_enable=true;rt.writeback_format=MALI_COLOR_FORMAT_R8G8B8A8;
    rt.writeback_block_format=MALI_BLOCK_FORMAT_LINEAR;rt.writeback_msaa=MALI_MSAA_SINGLE;
    rt.swizzle=0x688; // X,Y,Z,W, three bits per component (Mesa pan_translate_swizzle_4).
    rt.clean_tile_write_enable=true;
    rt.writeback_buffer.base=gpu+ImageOffset;
    rt.writeback_buffer.row_stride=Stride;rt.writeback_buffer.surface_stride=ImageBytes;
    rt.clear.color_0=rt.clear.color_1=rt.clear.color_2=rt.clear.color_3=Colors[quadrant];
    MALI_RGB_RENDER_TARGET_pack((mali_rgb_render_target_packed *)(p+FramebufferOffset+128),&rt);
    MALI_TILER_HEAP heap={};heap.size=0x2000;heap.base=heap.bottom=gpu+HeapOffset;
    heap.top=gpu+0x4000;
    MALI_TILER_HEAP_pack((mali_tiler_heap_packed *)(p+HeapDescOffset),&heap);
    MALI_TILER_CONTEXT tiler={};tiler.polygon_list=gpu+PolygonOffset;
    tiler.hierarchy_mask=1; // One 16x16 bin level; the 64x64 empty list fits one owned page.
    tiler.fb_width=Width;tiler.fb_height=Height;tiler.heap=gpu+HeapDescOffset;
    MALI_TILER_CONTEXT_pack((mali_tiler_context_packed *)(p+TilerOffset),&tiler);
    auto *samples=(uint16_t *)(p+SampleOffset);
    samples[0]=samples[1]=128;samples[64]=samples[65]=128;
    return true;
}

inline uint32_t expected(unsigned x,unsigned y) {return Colors[(x>=32?1:0)|(y>=32?2:0)];}
}
