// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;
kernel void nm_double(device float *p [[buffer(0)]],uint i [[thread_position_in_grid]])
{ if(i<64)p[i]=p[i]*2.0f+1.0f; }
vertex float4 nm_triangle(uint i [[vertex_id]])
{
    constexpr float2 corners[3]={float2(-0.75f,-0.75f),float2(0.75f,-0.75f),float2(0.0f,0.75f)};
    return float4(corners[i],0.0f,1.0f);
}
fragment float4 nm_red() { return float4(1.0f,0.0f,0.0f,1.0f); }
