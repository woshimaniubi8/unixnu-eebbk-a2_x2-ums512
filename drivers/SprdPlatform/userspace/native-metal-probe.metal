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
kernel void nm_read_surface(texture2d<float,access::read> image [[texture(0)]],
    device uint *output [[buffer(0)]],uint i [[thread_position_in_grid]])
{
    if(i>=4096)return;
    float4 p=image.read(uint2(i&63u,i>>6u));
    output[i]=(p.r>0.5f?255u:0u)|((p.g>0.5f?255u:0u)<<8u)|
        ((p.b>0.5f?255u:0u)<<16u)|((p.a>0.5f?255u:0u)<<24u);
}
