/* SPDX-License-Identifier: MIT */
/* Generated pack-only subset of Mesa26.2.6 v7.xml by its unmodified gen_pack.py.
 * Copyright (C) 2016 Intel Corporation, Broadcom; 2020 Collabora Ltd.
 * Reproduce with scripts/generate-mali-render-packets.py. No field offsets
 * or packing expressions are edited. Printing/unpacking code is omitted. */
#pragma once
#include <stdint.h>
#include <string.h>
#if defined(KERNEL)
#include <kern/assert.h>
#else
#include <assert.h>
#endif
#ifndef IS_POT_NONZERO
#define IS_POT_NONZERO(x) ((x) && !((x)&((x)-1)))
#endif
#ifndef ALIGN_POT
#define ALIGN_POT(x,a) (((x)+(a)-1)&~((a)-1))
#endif
namespace SprdMaliMesa {
// Adapted Mesa bitpack_helpers.h / libcl integer helpers, without libc/CL types.
inline uint64_t util_bitpack_uint(uint64_t v,unsigned start,unsigned end) {
    assert(start<=end && end<64);
    assert(end-start==63 || v<(UINT64_C(1)<<(end-start+1)));
    return v<<start;
}
inline unsigned util_logbase2(unsigned value) {assert(value);return 31u-__builtin_clz(value);}
inline uint32_t util_bitpack_float(float value) {uint32_t bits;memcpy(&bits,&value,4);return bits;}
enum mali_attribute_type {
        MALI_ATTRIBUTE_TYPE_1D               =      1,
        MALI_ATTRIBUTE_TYPE_1D_POT_DIVISOR   =      2,
        MALI_ATTRIBUTE_TYPE_1D_MODULUS       =      3,
        MALI_ATTRIBUTE_TYPE_1D_NPOT_DIVISOR  =      4,
        MALI_ATTRIBUTE_TYPE_3D_LINEAR        =      5,
        MALI_ATTRIBUTE_TYPE_3D_INTERLEAVED   =      6,
        MALI_ATTRIBUTE_TYPE_1D_PRIMITIVE_INDEX_BUFFER =      7,
        MALI_ATTRIBUTE_TYPE_1D_POT_DIVISOR_WRITE_REDUCTION =     10,
        MALI_ATTRIBUTE_TYPE_1D_MODULUS_WRITE_REDUCTION =     11,
        MALI_ATTRIBUTE_TYPE_1D_NPOT_DIVISOR_WRITE_REDUCTION =     12,
        MALI_ATTRIBUTE_TYPE_CONTINUATION     =     32,
};

enum mali_channel {
        MALI_CHANNEL_R                       =      0,
        MALI_CHANNEL_G                       =      1,
        MALI_CHANNEL_B                       =      2,
        MALI_CHANNEL_A                       =      3,
        MALI_CHANNEL_0                       =      4,
        MALI_CHANNEL_1                       =      5,
};

enum mali_depth_source {
        MALI_DEPTH_SOURCE_MINIMUM            =      0,
        MALI_DEPTH_SOURCE_MAXIMUM            =      1,
        MALI_DEPTH_SOURCE_FIXED_FUNCTION     =      2,
        MALI_DEPTH_SOURCE_SHADER             =      3,
};

enum mali_job_type {
        MALI_JOB_TYPE_NOT_STARTED            =      0,
        MALI_JOB_TYPE_NULL                   =      1,
        MALI_JOB_TYPE_WRITE_VALUE            =      2,
        MALI_JOB_TYPE_CACHE_FLUSH            =      3,
        MALI_JOB_TYPE_COMPUTE                =      4,
        MALI_JOB_TYPE_VERTEX                 =      5,
        MALI_JOB_TYPE_GEOMETRY               =      6,
        MALI_JOB_TYPE_TILER                  =      7,
        MALI_JOB_TYPE_FUSED                  =      8,
        MALI_JOB_TYPE_FRAGMENT               =      9,
        MALI_JOB_TYPE_INDEXED_VERTEX         =     10,
};

enum mali_draw_mode {
        MALI_DRAW_MODE_NONE                  =      0,
        MALI_DRAW_MODE_POINTS                =      1,
        MALI_DRAW_MODE_LINES                 =      2,
        MALI_DRAW_MODE_LINE_STRIP            =      4,
        MALI_DRAW_MODE_LINE_LOOP             =      6,
        MALI_DRAW_MODE_TRIANGLES             =      8,
        MALI_DRAW_MODE_TRIANGLE_STRIP        =     10,
        MALI_DRAW_MODE_TRIANGLE_FAN          =     12,
        MALI_DRAW_MODE_POLYGON               =     13,
        MALI_DRAW_MODE_QUADS                 =     14,
};

enum mali_exception_access {
        MALI_EXCEPTION_ACCESS_NONE           =      0,
        MALI_EXCEPTION_ACCESS_EXECUTE        =      2,
        MALI_EXCEPTION_ACCESS_READ           =      1,
        MALI_EXCEPTION_ACCESS_WRITE          =      3,
};

enum mali_func {
        MALI_FUNC_NEVER                      =      0,
        MALI_FUNC_LESS                       =      1,
        MALI_FUNC_EQUAL                      =      2,
        MALI_FUNC_LEQUAL                     =      3,
        MALI_FUNC_GREATER                    =      4,
        MALI_FUNC_NOT_EQUAL                  =      5,
        MALI_FUNC_GEQUAL                     =      6,
        MALI_FUNC_ALWAYS                     =      7,
};

enum mali_format {
        MALI_ETC2_RGB8                       =      1,
        MALI_ETC2_R11_UNORM                  =      2,
        MALI_ETC2_RGBA8                      =      3,
        MALI_ETC2_RG11_UNORM                 =      4,
        MALI_R4A4_UNORM                      =      5,
        MALI_A4R4_UNORM                      =      6,
        MALI_BC1_UNORM                       =      7,
        MALI_BC2_UNORM                       =      8,
        MALI_BC3_UNORM                       =      9,
        MALI_BC4_UNORM                       =     10,
        MALI_BC4_SNORM                       =     11,
        MALI_BC5_UNORM                       =     12,
        MALI_BC5_SNORM                       =     13,
        MALI_BC6H_UF16                       =     14,
        MALI_BC6H_SF16                       =     15,
        MALI_BC7_UNORM                       =     16,
        MALI_ETC2_R11_SNORM                  =     17,
        MALI_ETC2_RG11_SNORM                 =     18,
        MALI_ETC2_RGB8A1                     =     19,
        MALI_ASTC_3D_LDR                     =     20,
        MALI_ASTC_3D_HDR                     =     21,
        MALI_ASTC_2D_LDR                     =     22,
        MALI_ASTC_2D_HDR                     =     23,
        MALI_R8A8_UNORM                      =     24,
        MALI_A8R8_UNORM                      =     25,
        MALI_A8_UNORM                        =     26,
        MALI_RAW10                           =     28,
        MALI_RAW12                           =     29,
        MALI_R8G8B8G8                        =     30,
        MALI_G8R8G8B8                        =     31,
        MALI_YUV8                            =     32,
        MALI_YUVA8                           =     33,
        MALI_YUYV8                           =     34,
        MALI_VYUY8                           =     35,
        MALI_Y8_UV8_422                      =     36,
        MALI_Y8_U8_V8_422                    =     37,
        MALI_Y8_UV8_420                      =     38,
        MALI_Y8_U8_V8_420                    =     39,
        MALI_YUV10_A2                        =     40,
        MALI_A2_YUV10                        =     41,
        MALI_YUYAAYVYAA                      =     42,
        MALI_YUYV10                          =     44,
        MALI_VYUY10                          =     45,
        MALI_Y10_UV10_422                    =     46,
        MALI_Y10_UV10_420                    =     47,
        MALI_YUV16                           =     48,
        MALI_YUVA16                          =     49,
        MALI_YUYV16                          =     50,
        MALI_VYUY16                          =     51,
        MALI_Y10X6_U10X6V10X6_422            =     52,
        MALI_Y16_UV16_422                    =     53,
        MALI_Y10X6_U10X6V10X6_420            =     54,
        MALI_Y16_UV16_420                    =     55,
        MALI_Y8_UV8_BL_422                   =     56,
        MALI_Y8_UV8_BL_420                   =     57,
        MALI_CUSTOM_YUV_2                    =     58,
        MALI_CUSTOM_YUV_3                    =     59,
        MALI_CUSTOM_YUV_4                    =     60,
        MALI_CUSTOM_YUV_5                    =     61,
        MALI_ASTC_2D_NARROW                  =     62,
        MALI_ASTC_3D_NARROW                  =     63,
        MALI_RGB565                          =     64,
        MALI_RGB5_A1_UNORM                   =     65,
        MALI_A1_BGR5_UNORM                   =     66,
        MALI_RGB10_A2_UNORM                  =     67,
        MALI_A2_BGR10_UNORM                  =     68,
        MALI_RGB10_A2_SNORM                  =     69,
        MALI_A2_BGR10_SNORM                  =     70,
        MALI_RGB10_A2UI                      =     71,
        MALI_A2_BGR10UI                      =     72,
        MALI_RGB10_A2I                       =     73,
        MALI_A2_BGR10I                       =     74,
        MALI_Z16_UNORM                       =     75,
        MALI_Z32                             =     76,
        MALI_Z24X8_UNORM                     =     77,
        MALI_X8Z24                           =     78,
        MALI_X32_S8X24                       =     79,
        MALI_X24S8_X32                       =     80,
        MALI_R32_FIXED                       =     81,
        MALI_RG32_FIXED                      =     82,
        MALI_RGB32_FIXED                     =     83,
        MALI_RGBA32_FIXED                    =     84,
        MALI_X24S8                           =     85,
        MALI_S8X24                           =     86,
        MALI_Z32_X32                         =     87,
        MALI_X32_Z32                         =     88,
        MALI_R11F_G11F_B10F                  =     89,
        MALI_B10F_G11F_R11F                  =     90,
        MALI_R9F_G9F_B9F_E5F                 =     91,
        MALI_E5F_B9F_G9F_R9F                 =     92,
        MALI_S8                              =     93,
        MALI_SNAP_4                          =     94,
        MALI_CONSTANT                        =     95,
        MALI_R8_SNORM                        =     99,
        MALI_R16_SNORM                       =    100,
        MALI_R32_SNORM                       =    101,
        MALI_RG4_SNORM                       =    106,
        MALI_RG8_SNORM                       =    107,
        MALI_RG16_SNORM                      =    108,
        MALI_RG32_SNORM                      =    109,
        MALI_RGB8_SNORM                      =    115,
        MALI_RGB16_SNORM                     =    116,
        MALI_RGB32_SNORM                     =    117,
        MALI_RGBA2_SNORM                     =    121,
        MALI_RGBA4_SNORM                     =    122,
        MALI_RGBA8_SNORM                     =    123,
        MALI_RGBA16_SNORM                    =    124,
        MALI_RGBA32_SNORM                    =    125,
        MALI_R8UI                            =    131,
        MALI_R16UI                           =    132,
        MALI_R32UI                           =    133,
        MALI_RG4UI                           =    138,
        MALI_RG8UI                           =    139,
        MALI_RG16UI                          =    140,
        MALI_RG32UI                          =    141,
        MALI_RGB8UI                          =    147,
        MALI_RGB16UI                         =    148,
        MALI_RGB32UI                         =    149,
        MALI_RGBA2UI                         =    153,
        MALI_RGBA4UI                         =    154,
        MALI_RGBA8UI                         =    155,
        MALI_RGBA16UI                        =    156,
        MALI_RGBA32UI                        =    157,
        MALI_R8_UNORM                        =    163,
        MALI_R16_UNORM                       =    164,
        MALI_R32_UNORM                       =    165,
        MALI_R32F                            =    167,
        MALI_RG4_UNORM                       =    170,
        MALI_RG8_UNORM                       =    171,
        MALI_RG16_UNORM                      =    172,
        MALI_RG32_UNORM                      =    173,
        MALI_RG32F                           =    175,
        MALI_RGB8_UNORM                      =    179,
        MALI_RGB16_UNORM                     =    180,
        MALI_RGB32_UNORM                     =    181,
        MALI_RGB32F                          =    183,
        MALI_RGBA2_UNORM                     =    185,
        MALI_RGBA4_UNORM                     =    186,
        MALI_RGBA8_UNORM                     =    187,
        MALI_RGBA16_UNORM                    =    188,
        MALI_RGBA32_UNORM                    =    189,
        MALI_RGBA32F                         =    191,
        MALI_R8I                             =    195,
        MALI_R16I                            =    196,
        MALI_R32I                            =    197,
        MALI_R16F                            =    199,
        MALI_RG4I                            =    202,
        MALI_RG8I                            =    203,
        MALI_RG16I                           =    204,
        MALI_RG32I                           =    205,
        MALI_RG16F                           =    207,
        MALI_RGB8I                           =    211,
        MALI_RGB16I                          =    212,
        MALI_RGB32I                          =    213,
        MALI_RGB16F                          =    215,
        MALI_RGBA2I                          =    217,
        MALI_RGBA4I                          =    218,
        MALI_RGBA8I                          =    219,
        MALI_RGBA16I                         =    220,
        MALI_RGBA32I                         =    221,
        MALI_RGBA16F                         =    223,
        MALI_RGB5_A1_AU                      =    224,
        MALI_RGB5_A1_PU                      =    225,
        MALI_R5G6B5_AU                       =    226,
        MALI_R5G6B5_PU                       =    227,
        MALI_SNAP4_V                         =    230,
        MALI_R32F_RTZ                        =    231,
        MALI_RGBA4_AU                        =    232,
        MALI_RGBA4_PU                        =    233,
        MALI_RGBA8_TB                        =    237,
        MALI_RGB10_A2_TB                     =    238,
        MALI_RG32F_RTZ                       =    239,
        MALI_TESS_VERTEX_PACK                =    240,
        MALI_RGB8_A2_AU                      =    241,
        MALI_RGB8_A2_PU                      =    242,
        MALI_RGB32F_RTZ                      =    247,
        MALI_RGBA32F_RTZ                     =    255,
};

enum mali_rgb_component_order {
        MALI_RGB_COMPONENT_ORDER_RGBA        =      0,
        MALI_RGB_COMPONENT_ORDER_GRBA        =      2,
        MALI_RGB_COMPONENT_ORDER_BGRA        =      4,
        MALI_RGB_COMPONENT_ORDER_ARGB        =      8,
        MALI_RGB_COMPONENT_ORDER_AGRB        =     10,
        MALI_RGB_COMPONENT_ORDER_ABGR        =     12,
        MALI_RGB_COMPONENT_ORDER_RGB1        =     16,
        MALI_RGB_COMPONENT_ORDER_GRB1        =     18,
        MALI_RGB_COMPONENT_ORDER_BGR1        =     20,
        MALI_RGB_COMPONENT_ORDER_1RGB        =     24,
        MALI_RGB_COMPONENT_ORDER_1GRB        =     26,
        MALI_RGB_COMPONENT_ORDER_1BGR        =     28,
        MALI_RGB_COMPONENT_ORDER_RRRR        =    226,
        MALI_RGB_COMPONENT_ORDER_RRR1        =    227,
        MALI_RGB_COMPONENT_ORDER_RRRA        =    228,
        MALI_RGB_COMPONENT_ORDER_000A        =    229,
        MALI_RGB_COMPONENT_ORDER_0001        =    230,
        MALI_RGB_COMPONENT_ORDER_0000        =    231,
        MALI_RGB_COMPONENT_ORDER_SNAP4_V9    =    232,
        MALI_RGB_COMPONENT_ORDER_SNAP4_V10   =    233,
        MALI_RGB_COMPONENT_ORDER_SNAP4_V11   =    234,
        MALI_RGB_COMPONENT_ORDER_SNAP4_V12   =    235,
        MALI_RGB_COMPONENT_ORDER_SNAP4_V13   =    236,
        MALI_RGB_COMPONENT_ORDER_SNAP4_V14   =    237,
        MALI_RGB_COMPONENT_ORDER_SNAP4_V15   =    238,
        MALI_RGB_COMPONENT_ORDER_SNAP4_V16   =    239,
        MALI_RGB_COMPONENT_ORDER_R000        =    240,
        MALI_RGB_COMPONENT_ORDER_RBGA        =    242,
};

enum mali_yuv_swizzle {
        MALI_YUV_SWIZZLE_YUVA                =      0,
        MALI_YUV_SWIZZLE_YVUA                =      1,
        MALI_YUV_SWIZZLE_UYVA                =      2,
        MALI_YUV_SWIZZLE_UVYA                =      3,
        MALI_YUV_SWIZZLE_VUYA                =      4,
        MALI_YUV_SWIZZLE_VYUA                =      5,
        MALI_YUV_SWIZZLE_Y00A                =      6,
        MALI_YUV_SWIZZLE_YXXA                =      7,
};

enum mali_yuv_conversion_mode {
        MALI_YUV_CONVERSION_MODE_NO_CONVERSION =      0,
        MALI_YUV_CONVERSION_MODE_BT_601      =      3,
        MALI_YUV_CONVERSION_MODE_BT_709      =      4,
        MALI_YUV_CONVERSION_MODE_BT_2020     =      6,
};

enum mali_yuv_cr_siting {
        MALI_YUV_CR_SITING_CO_SITED          =      0,
        MALI_YUV_CR_SITING_CENTER_Y          =      1,
        MALI_YUV_CR_SITING_CENTER_X          =      2,
        MALI_YUV_CR_SITING_CENTER            =      3,
        MALI_YUV_CR_SITING_ONE_QUARTER       =      4,
        MALI_YUV_CR_SITING_THREE_QUARTERS    =      5,
        MALI_YUV_CR_SITING_REPLICATED        =      7,
};

enum mali_astc_2d_dimension {
        MALI_ASTC_2D_DIMENSION_4             =      0,
        MALI_ASTC_2D_DIMENSION_5             =      1,
        MALI_ASTC_2D_DIMENSION_6             =      2,
        MALI_ASTC_2D_DIMENSION_8             =      4,
        MALI_ASTC_2D_DIMENSION_10            =      6,
        MALI_ASTC_2D_DIMENSION_12            =      7,
};

enum mali_astc_3d_dimension {
        MALI_ASTC_3D_DIMENSION_4             =      0,
        MALI_ASTC_3D_DIMENSION_5             =      1,
        MALI_ASTC_3D_DIMENSION_6             =      2,
        MALI_ASTC_3D_DIMENSION_3             =      3,
};

enum mali_pixel_kill {
        MALI_PIXEL_KILL_FORCE_EARLY          =      0,
        MALI_PIXEL_KILL_STRONG_EARLY         =      1,
        MALI_PIXEL_KILL_WEAK_EARLY           =      2,
        MALI_PIXEL_KILL_FORCE_LATE           =      3,
};

enum mali_block_format {
        MALI_BLOCK_FORMAT_NO_WRITE           =      0,
        MALI_BLOCK_FORMAT_TILED_U_INTERLEAVED =      1,
        MALI_BLOCK_FORMAT_LINEAR             =      2,
        MALI_BLOCK_FORMAT_AFBC               =     12,
        MALI_BLOCK_FORMAT_AFBC_TILED         =     13,
};

enum mali_mipmap_mode {
        MALI_MIPMAP_MODE_NEAREST             =      0,
        MALI_MIPMAP_MODE_NONE                =      1,
        MALI_MIPMAP_MODE_PERFORMANCE_TRILINEAR =      2,
        MALI_MIPMAP_MODE_TRILINEAR           =      3,
};

enum mali_lod_algorithm {
        MALI_LOD_ALGORITHM_ISOTROPIC         =      0,
        MALI_LOD_ALGORITHM_ANISOTROPIC       =      3,
};

enum mali_msaa {
        MALI_MSAA_SINGLE                     =      0,
        MALI_MSAA_AVERAGE                    =      1,
        MALI_MSAA_MULTIPLE                   =      2,
        MALI_MSAA_LAYERED                    =      3,
};

enum mali_index_type {
        MALI_INDEX_TYPE_NONE                 =      0,
        MALI_INDEX_TYPE_UINT8                =      1,
        MALI_INDEX_TYPE_UINT16               =      2,
        MALI_INDEX_TYPE_UINT32               =      3,
};

enum mali_occlusion_mode {
        MALI_OCCLUSION_MODE_DISABLED         =      0,
        MALI_OCCLUSION_MODE_PREDICATE        =      1,
        MALI_OCCLUSION_MODE_COUNTER          =      3,
};

enum mali_stencil_op {
        MALI_STENCIL_OP_KEEP                 =      0,
        MALI_STENCIL_OP_REPLACE              =      1,
        MALI_STENCIL_OP_ZERO                 =      2,
        MALI_STENCIL_OP_INVERT               =      3,
        MALI_STENCIL_OP_INCR_WRAP            =      4,
        MALI_STENCIL_OP_DECR_WRAP            =      5,
        MALI_STENCIL_OP_INCR_SAT             =      6,
        MALI_STENCIL_OP_DECR_SAT             =      7,
};

enum mali_texture_dimension {
        MALI_TEXTURE_DIMENSION_CUBE          =      0,
        MALI_TEXTURE_DIMENSION_1D            =      1,
        MALI_TEXTURE_DIMENSION_2D            =      2,
        MALI_TEXTURE_DIMENSION_3D            =      3,
};

enum mali_texture_layout {
        MALI_TEXTURE_LAYOUT_TILED            =      1,
        MALI_TEXTURE_LAYOUT_LINEAR           =      2,
        MALI_TEXTURE_LAYOUT_AFBC             =     12,
};

enum mali_afbc_surface_flag {
        MALI_AFBC_SURFACE_FLAG_YTR           =      1,
        MALI_AFBC_SURFACE_FLAG_SPLIT_BLOCK   =      2,
        MALI_AFBC_SURFACE_FLAG_WIDE_BLOCK    =      4,
        MALI_AFBC_SURFACE_FLAG_TILED_HEADER  =      8,
        MALI_AFBC_SURFACE_FLAG_PREFETCH      =     16,
        MALI_AFBC_SURFACE_FLAG_CHECK_PAYLOAD_RANGE =     32,
};

enum mali_wrap_mode {
        MALI_WRAP_MODE_REPEAT                =      8,
        MALI_WRAP_MODE_CLAMP_TO_EDGE         =      9,
        MALI_WRAP_MODE_CLAMP_TO_BORDER       =     11,
        MALI_WRAP_MODE_MIRRORED_REPEAT       =     12,
        MALI_WRAP_MODE_MIRRORED_CLAMP_TO_EDGE =     13,
        MALI_WRAP_MODE_MIRRORED_CLAMP_TO_BORDER =     15,
};

enum mali_blend_operand_a {
        MALI_BLEND_OPERAND_A_ZERO            =      1,
        MALI_BLEND_OPERAND_A_SRC             =      2,
        MALI_BLEND_OPERAND_A_DEST            =      3,
};

enum mali_blend_operand_b {
        MALI_BLEND_OPERAND_B_SRC_MINUS_DEST  =      0,
        MALI_BLEND_OPERAND_B_SRC_PLUS_DEST   =      1,
        MALI_BLEND_OPERAND_B_SRC             =      2,
        MALI_BLEND_OPERAND_B_DEST            =      3,
};

enum mali_blend_operand_c {
        MALI_BLEND_OPERAND_C_ZERO            =      1,
        MALI_BLEND_OPERAND_C_SRC             =      2,
        MALI_BLEND_OPERAND_C_DEST            =      3,
        MALI_BLEND_OPERAND_C_SRC_X_2         =      4,
        MALI_BLEND_OPERAND_C_SRC_ALPHA       =      5,
        MALI_BLEND_OPERAND_C_DEST_ALPHA      =      6,
        MALI_BLEND_OPERAND_C_CONSTANT        =      7,
};

enum mali_register_file_format {
        MALI_REGISTER_FILE_FORMAT_F16        =      0,
        MALI_REGISTER_FILE_FORMAT_F32        =      1,
        MALI_REGISTER_FILE_FORMAT_I32        =      2,
        MALI_REGISTER_FILE_FORMAT_U32        =      3,
        MALI_REGISTER_FILE_FORMAT_I16        =      4,
        MALI_REGISTER_FILE_FORMAT_U16        =      5,
};

enum mali_message_type {
        MALI_MESSAGE_TYPE_DISABLED           =      0,
        MALI_MESSAGE_TYPE_LD_VAR             =      1,
        MALI_MESSAGE_TYPE_VAR_TEX            =      2,
};

enum mali_message_preload_register_format {
        MALI_MESSAGE_PRELOAD_REGISTER_FORMAT_F32 =      0,
        MALI_MESSAGE_PRELOAD_REGISTER_FORMAT_F16 =      1,
};

enum mali_blend_mode {
        MALI_BLEND_MODE_SHADER               =      0,
        MALI_BLEND_MODE_OPAQUE               =      1,
        MALI_BLEND_MODE_FIXED_FUNCTION       =      2,
        MALI_BLEND_MODE_OFF                  =      3,
};

enum mali_point_size_array_format {
        MALI_POINT_SIZE_ARRAY_FORMAT_NONE    =      0,
        MALI_POINT_SIZE_ARRAY_FORMAT_FP16    =      2,
        MALI_POINT_SIZE_ARRAY_FORMAT_FP32    =      3,
};

enum mali_primitive_restart {
        MALI_PRIMITIVE_RESTART_NONE          =      0,
        MALI_PRIMITIVE_RESTART_IMPLICIT      =      2,
        MALI_PRIMITIVE_RESTART_EXPLICIT      =      3,
};

enum mali_shader_register_allocation {
        MALI_SHADER_REGISTER_ALLOCATION_64_PER_THREAD =      0,
        MALI_SHADER_REGISTER_ALLOCATION_32_PER_THREAD =      2,
};

enum mali_warp_limit {
        MALI_WARP_LIMIT_NONE                 =      0,
        MALI_WARP_LIMIT_2                    =      1,
        MALI_WARP_LIMIT_4                    =      2,
        MALI_WARP_LIMIT_8                    =      3,
};

enum mali_color_buffer_internal_format {
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_RAW_VALUE =      0,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8 =      1,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_R10G10B10A2 =      2,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A2 =      3,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_R4G4B4A4 =      4,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_R5G6B5A0 =      5,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_R5G5B5A1 =      6,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_RAW8 =     32,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_RAW16 =     33,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_RAW32 =     34,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_RAW64 =     35,
        MALI_COLOR_BUFFER_INTERNAL_FORMAT_RAW128 =     36,
};

enum mali_color_format {
        MALI_COLOR_FORMAT_RAW8               =      0,
        MALI_COLOR_FORMAT_RAW16              =      1,
        MALI_COLOR_FORMAT_RAW24              =      2,
        MALI_COLOR_FORMAT_RAW32              =      3,
        MALI_COLOR_FORMAT_RAW48              =      4,
        MALI_COLOR_FORMAT_RAW64              =      5,
        MALI_COLOR_FORMAT_RAW96              =      6,
        MALI_COLOR_FORMAT_RAW128             =      7,
        MALI_COLOR_FORMAT_RAW192             =      8,
        MALI_COLOR_FORMAT_RAW256             =      9,
        MALI_COLOR_FORMAT_RAW384             =     10,
        MALI_COLOR_FORMAT_RAW512             =     11,
        MALI_COLOR_FORMAT_RAW768             =     12,
        MALI_COLOR_FORMAT_RAW1024            =     13,
        MALI_COLOR_FORMAT_RAW1536            =     14,
        MALI_COLOR_FORMAT_RAW2048            =     15,
        MALI_COLOR_FORMAT_R8                 =     16,
        MALI_COLOR_FORMAT_R8G8               =     17,
        MALI_COLOR_FORMAT_R8G8B8             =     18,
        MALI_COLOR_FORMAT_R8G8B8A8           =     19,
        MALI_COLOR_FORMAT_R4G4B4A4           =     20,
        MALI_COLOR_FORMAT_R5G6B5             =     21,
        MALI_COLOR_FORMAT_R8G8B8_FROM_R8G8B8A2 =     22,
        MALI_COLOR_FORMAT_R10G10B10A2        =     24,
        MALI_COLOR_FORMAT_A2B10G10R10        =     25,
        MALI_COLOR_FORMAT_R5G5B5A1           =     28,
        MALI_COLOR_FORMAT_A1B5G5R5           =     29,
        MALI_COLOR_FORMAT_NATIVE             =     31,
};

enum mali_downsampling_accumulation_mode {
        MALI_DOWNSAMPLING_ACCUMULATION_MODE_UNSIGNED_NORMALIZED_INTEGER =      0,
        MALI_DOWNSAMPLING_ACCUMULATION_MODE_SIGNED_NORMALIZED_INTEGER =      1,
};

enum mali_sample_layout {
        MALI_SAMPLE_LAYOUT_ORDERED_4X_GRID   =      0,
        MALI_SAMPLE_LAYOUT_ROTATED_4X_GRID   =      1,
        MALI_SAMPLE_LAYOUT_D3D_8X_GRID       =      2,
        MALI_SAMPLE_LAYOUT_D3D_16X_GRID      =      3,
};

enum mali_zs_format {
        MALI_ZS_FORMAT_D16                   =      1,
        MALI_ZS_FORMAT_D24                   =      2,
        MALI_ZS_FORMAT_D24X8                 =      4,
        MALI_ZS_FORMAT_D24S8                 =      5,
        MALI_ZS_FORMAT_X8D24                 =      6,
        MALI_ZS_FORMAT_S8D24                 =      7,
        MALI_ZS_FORMAT_D32_X8X24             =     13,
        MALI_ZS_FORMAT_D32                   =     14,
        MALI_ZS_FORMAT_D32_S8X24             =     15,
};

enum mali_zs_preload_format {
        MALI_ZS_PRELOAD_FORMAT_D32_S8X24     =      4,
};

enum mali_s_format {
        MALI_S_FORMAT_S8                     =      1,
        MALI_S_FORMAT_S8X8                   =      2,
        MALI_S_FORMAT_S8X24                  =      3,
        MALI_S_FORMAT_X24S8                  =      4,
        MALI_S_FORMAT_X8S8                   =      5,
        MALI_S_FORMAT_X32_S8X24              =      6,
};

enum mali_tie_break_rule {
        MALI_TIE_BREAK_RULE_0_IN_180_OUT     =      0,
        MALI_TIE_BREAK_RULE_0_OUT_180_IN     =      1,
        MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT =      2,
        MALI_TIE_BREAK_RULE_MINUS_180_OUT_0_IN =      3,
        MALI_TIE_BREAK_RULE_90_IN_270_OUT    =      4,
        MALI_TIE_BREAK_RULE_90_OUT_270_IN    =      5,
        MALI_TIE_BREAK_RULE_MINUS_90_IN_90_OUT =      6,
        MALI_TIE_BREAK_RULE_MINUS_90_OUT_90_IN =      7,
};

enum mali_sample_pattern {
        MALI_SAMPLE_PATTERN_SINGLE_SAMPLED   =      0,
        MALI_SAMPLE_PATTERN_ORDERED_4X_GRID  =      1,
        MALI_SAMPLE_PATTERN_ROTATED_4X_GRID  =      2,
        MALI_SAMPLE_PATTERN_D3D_8X_GRID      =      3,
        MALI_SAMPLE_PATTERN_D3D_16X_GRID     =      4,
};

enum mali_z_internal_format {
        MALI_Z_INTERNAL_FORMAT_D16           =      0,
        MALI_Z_INTERNAL_FORMAT_D24           =      1,
        MALI_Z_INTERNAL_FORMAT_D32           =      2,
};

enum mali_pre_post_frame_shader_mode {
        MALI_PRE_POST_FRAME_SHADER_MODE_NEVER =      0,
        MALI_PRE_POST_FRAME_SHADER_MODE_ALWAYS =      1,
        MALI_PRE_POST_FRAME_SHADER_MODE_INTERSECT =      2,
        MALI_PRE_POST_FRAME_SHADER_MODE_EARLY_ZS_ALWAYS =      3,
};

enum mali_yuv_conv_k6 {
        MALI_YUV_CONV_K6_0                   =      0,
        MALI_YUV_CONV_K6_16                  =      1,
};

enum mali_yuv_conv_k7_clamp {
        MALI_YUV_CONV_K7_CLAMP_MINUS_128_TO_127 =      0,
        MALI_YUV_CONV_K7_CLAMP_MINUS_112_TO_111 =      1,
        MALI_YUV_CONV_K7_CLAMP_0_TO_255      =      2,
        MALI_YUV_CONV_K7_CLAMP_16_TO_239     =      3,
};

enum mali_yuv_conv_k8 {
        MALI_YUV_CONV_K8_220                 =      0,
        MALI_YUV_CONV_K8_256                 =      1,
};

enum mali_write_value_type {
        MALI_WRITE_VALUE_TYPE_CYCLE_COUNTER  =      1,
        MALI_WRITE_VALUE_TYPE_SYSTEM_TIMESTAMP =      2,
        MALI_WRITE_VALUE_TYPE_ZERO           =      3,
        MALI_WRITE_VALUE_TYPE_IMMEDIATE_8    =      4,
        MALI_WRITE_VALUE_TYPE_IMMEDIATE_16   =      5,
        MALI_WRITE_VALUE_TYPE_IMMEDIATE_32   =      6,
        MALI_WRITE_VALUE_TYPE_IMMEDIATE_64   =      7,
};

struct MALI_JOB_HEADER {
   uint64_t                             exception_status;
   uint64_t                             first_incomplete_task;
   uint64_t                             fault_pointer;
   bool                                 is_64b;
   enum mali_job_type                   type;
   bool                                 barrier;
   bool                                 invalidate_cache;
   bool                                 suppress_prefetch;
   bool                                 enable_texture_mapper;
   bool                                 relax_dependency_1;
   bool                                 relax_dependency_2;
   uint32_t                             index;
   uint32_t                             dependency_1;
   uint32_t                             dependency_2;
   uint64_t                             next;
};

#define MALI_JOB_HEADER_header                  \
   .is_64b = true

#define MALI_JOB_HEADER_LENGTH 32
#define MALI_JOB_HEADER_ALIGN 64
struct mali_job_header_packed { uint32_t opaque[8]; };
#define MALI_JOB_HEADER_PACKED_T struct mali_job_header_packed
#define MALI_JOB_HEADER_pack(__packed, __unpacked) \
do { \
   MALI_JOB_HEADER_PACKED_T __tmp_packed; \
   __tmp_packed.opaque[ 0] = util_bitpack_uint((__unpacked)->exception_status, 0, 31); \
   __tmp_packed.opaque[ 1] = util_bitpack_uint((__unpacked)->first_incomplete_task, 0, 31); \
   __tmp_packed.opaque[ 2] = util_bitpack_uint((__unpacked)->fault_pointer, 0, 63); \
   __tmp_packed.opaque[ 3] = util_bitpack_uint((__unpacked)->fault_pointer, 0, 63) >> 32; \
   __tmp_packed.opaque[ 4] = util_bitpack_uint((__unpacked)->is_64b, 0, 0) | \
            util_bitpack_uint((__unpacked)->type, 1, 7) | \
            util_bitpack_uint((__unpacked)->barrier, 8, 8) | \
            util_bitpack_uint((__unpacked)->invalidate_cache, 9, 9) | \
            util_bitpack_uint((__unpacked)->suppress_prefetch, 11, 11) | \
            util_bitpack_uint((__unpacked)->enable_texture_mapper, 12, 12) | \
            util_bitpack_uint((__unpacked)->relax_dependency_1, 14, 14) | \
            util_bitpack_uint((__unpacked)->relax_dependency_2, 15, 15) | \
            util_bitpack_uint((__unpacked)->index, 16, 31); \
   __tmp_packed.opaque[ 5] = util_bitpack_uint((__unpacked)->dependency_1, 0, 15) | \
            util_bitpack_uint((__unpacked)->dependency_2, 16, 31); \
   __tmp_packed.opaque[ 6] = util_bitpack_uint((__unpacked)->next, 0, 63); \
   __tmp_packed.opaque[ 7] = util_bitpack_uint((__unpacked)->next, 0, 63) >> 32; \
   *(__packed) = __tmp_packed; \
} while (0);



struct MALI_FRAMEBUFFER_PARAMETERS {
   enum mali_pre_post_frame_shader_mode pre_frame_0;
   enum mali_pre_post_frame_shader_mode pre_frame_1;
   enum mali_pre_post_frame_shader_mode post_frame;
   uint64_t                             sample_locations;
   uint64_t                             frame_shader_dcds;
   uint32_t                             width;
   uint32_t                             height;
   uint32_t                             bound_min_x;
   uint32_t                             bound_min_y;
   uint32_t                             bound_max_x;
   uint32_t                             bound_max_y;
   uint32_t                             sample_count;
   enum mali_sample_pattern             sample_pattern;
   enum mali_tie_break_rule             tie_break_rule;
   uint32_t                             effective_tile_size;
   uint32_t                             x_downsampling_scale;
   uint32_t                             y_downsampling_scale;
   uint32_t                             render_target_count;
   uint32_t                             color_buffer_allocation;
   uint32_t                             s_clear;
   bool                                 s_write_enable;
   bool                                 s_preload_enable;
   bool                                 s_unload_enable;
   enum mali_z_internal_format          z_internal_format;
   bool                                 z_write_enable;
   bool                                 z_preload_enable;
   bool                                 z_unload_enable;
   bool                                 has_zs_crc_extension;
   bool                                 empty_tile_read_enable;
   bool                                 empty_tile_write_enable;
   bool                                 crc_read_enable;
   bool                                 crc_write_enable;
   float                                z_clear;
   uint64_t                             tiler;
};

#define MALI_FRAMEBUFFER_PARAMETERS_header      \
   .sample_count = 1

#define MALI_FRAMEBUFFER_PARAMETERS_LENGTH 64
struct mali_framebuffer_parameters_packed { uint32_t opaque[16]; };
#define MALI_FRAMEBUFFER_PARAMETERS_PACKED_T struct mali_framebuffer_parameters_packed
#define MALI_FRAMEBUFFER_PARAMETERS_pack(__packed, __unpacked) \
do { \
   MALI_FRAMEBUFFER_PARAMETERS_PACKED_T __tmp_packed; \
   assert((__unpacked)->width >= 1); \
   assert((__unpacked)->height >= 1); \
   assert(IS_POT_NONZERO((__unpacked)->sample_count)); \
   assert(IS_POT_NONZERO((__unpacked)->effective_tile_size)); \
   assert((__unpacked)->render_target_count >= 1); \
   assert(((__unpacked)->color_buffer_allocation & 0x3ff) == 0); \
   __tmp_packed.opaque[ 0] = util_bitpack_uint((__unpacked)->pre_frame_0, 0, 2) | \
            util_bitpack_uint((__unpacked)->pre_frame_1, 3, 5) | \
            util_bitpack_uint((__unpacked)->post_frame, 6, 8); \
   __tmp_packed.opaque[ 1] = 0; \
   __tmp_packed.opaque[ 2] = 0; \
   __tmp_packed.opaque[ 3] = 0; \
   __tmp_packed.opaque[ 4] = util_bitpack_uint((__unpacked)->sample_locations, 0, 63); \
   __tmp_packed.opaque[ 5] = util_bitpack_uint((__unpacked)->sample_locations, 0, 63) >> 32; \
   __tmp_packed.opaque[ 6] = util_bitpack_uint((__unpacked)->frame_shader_dcds, 0, 63); \
   __tmp_packed.opaque[ 7] = util_bitpack_uint((__unpacked)->frame_shader_dcds, 0, 63) >> 32; \
   __tmp_packed.opaque[ 8] = util_bitpack_uint((__unpacked)->width - 1, 0, 15) | \
            util_bitpack_uint((__unpacked)->height - 1, 16, 31); \
   __tmp_packed.opaque[ 9] = util_bitpack_uint((__unpacked)->bound_min_x, 0, 15) | \
            util_bitpack_uint((__unpacked)->bound_min_y, 16, 31); \
   __tmp_packed.opaque[10] = util_bitpack_uint((__unpacked)->bound_max_x, 0, 15) | \
            util_bitpack_uint((__unpacked)->bound_max_y, 16, 31); \
   __tmp_packed.opaque[11] = util_bitpack_uint(util_logbase2((__unpacked)->sample_count), 0, 2) | \
            util_bitpack_uint((__unpacked)->sample_pattern, 3, 5) | \
            util_bitpack_uint((__unpacked)->tie_break_rule, 6, 8) | \
            util_bitpack_uint(util_logbase2((__unpacked)->effective_tile_size), 9, 12) | \
            util_bitpack_uint((__unpacked)->x_downsampling_scale, 13, 15) | \
            util_bitpack_uint((__unpacked)->y_downsampling_scale, 16, 18) | \
            util_bitpack_uint((__unpacked)->render_target_count - 1, 19, 22) | \
            util_bitpack_uint((__unpacked)->color_buffer_allocation >> 10, 24, 31); \
   __tmp_packed.opaque[12] = util_bitpack_uint((__unpacked)->s_clear, 0, 7) | \
            util_bitpack_uint((__unpacked)->s_write_enable, 8, 8) | \
            util_bitpack_uint((__unpacked)->s_preload_enable, 9, 9) | \
            util_bitpack_uint((__unpacked)->s_unload_enable, 10, 10) | \
            util_bitpack_uint((__unpacked)->z_internal_format, 16, 17) | \
            util_bitpack_uint((__unpacked)->z_write_enable, 18, 18) | \
            util_bitpack_uint((__unpacked)->z_preload_enable, 19, 19) | \
            util_bitpack_uint((__unpacked)->z_unload_enable, 20, 20) | \
            util_bitpack_uint((__unpacked)->has_zs_crc_extension, 21, 21) | \
            util_bitpack_uint((__unpacked)->empty_tile_read_enable, 28, 28) | \
            util_bitpack_uint((__unpacked)->empty_tile_write_enable, 29, 29) | \
            util_bitpack_uint((__unpacked)->crc_read_enable, 30, 30) | \
            util_bitpack_uint((__unpacked)->crc_write_enable, 31, 31); \
   __tmp_packed.opaque[13] = util_bitpack_float((__unpacked)->z_clear); \
   __tmp_packed.opaque[14] = util_bitpack_uint((__unpacked)->tiler, 0, 63); \
   __tmp_packed.opaque[15] = util_bitpack_uint((__unpacked)->tiler, 0, 63) >> 32; \
   *(__packed) = __tmp_packed; \
} while (0);



struct MALI_RT_BUFFER {
   uint64_t                             base;
   uint64_t                             row_stride;
   uint64_t                             surface_stride;
};

#define MALI_RT_BUFFER_header                   \
   0

#define MALI_RT_BUFFER_LENGTH 16
struct mali_rt_buffer_packed { uint32_t opaque[4]; };
#define MALI_RT_BUFFER_PACKED_T struct mali_rt_buffer_packed
#define MALI_RT_BUFFER_pack(__packed, __unpacked) \
do { \
   MALI_RT_BUFFER_PACKED_T __tmp_packed; \
   __tmp_packed.opaque[ 0] = util_bitpack_uint((__unpacked)->base, 0, 63); \
   __tmp_packed.opaque[ 1] = util_bitpack_uint((__unpacked)->base, 0, 63) >> 32; \
   __tmp_packed.opaque[ 2] = util_bitpack_uint((__unpacked)->row_stride, 0, 31); \
   __tmp_packed.opaque[ 3] = util_bitpack_uint((__unpacked)->surface_stride, 0, 31); \
   *(__packed) = __tmp_packed; \
} while (0);



struct MALI_RT_CLEAR {
   uint64_t                             color_0;
   uint64_t                             color_1;
   uint64_t                             color_2;
   uint64_t                             color_3;
};

#define MALI_RT_CLEAR_header                    \
   0

#define MALI_RT_CLEAR_LENGTH 16
struct mali_rt_clear_packed { uint32_t opaque[4]; };
#define MALI_RT_CLEAR_PACKED_T struct mali_rt_clear_packed
#define MALI_RT_CLEAR_pack(__packed, __unpacked) \
do { \
   MALI_RT_CLEAR_PACKED_T __tmp_packed; \
   __tmp_packed.opaque[ 0] = util_bitpack_uint((__unpacked)->color_0, 0, 31); \
   __tmp_packed.opaque[ 1] = util_bitpack_uint((__unpacked)->color_1, 0, 31); \
   __tmp_packed.opaque[ 2] = util_bitpack_uint((__unpacked)->color_2, 0, 31); \
   __tmp_packed.opaque[ 3] = util_bitpack_uint((__unpacked)->color_3, 0, 31); \
   *(__packed) = __tmp_packed; \
} while (0);



struct MALI_RGB_RENDER_TARGET {
   uint32_t                             internal_buffer_offset;
   bool                                 yuv_enable;
   bool                                 dithered_clear;
   enum mali_color_buffer_internal_format internal_format;
   bool                                 write_enable;
   enum mali_color_format               writeback_format;
   enum mali_block_format               writeback_block_format;
   enum mali_msaa                       writeback_msaa;
   bool                                 srgb;
   bool                                 dithering_enable;
   uint32_t                             swizzle;
   bool                                 clean_tile_write_enable;
   struct MALI_RT_BUFFER                writeback_buffer;
   struct MALI_RT_CLEAR                 clear;
};

#define MALI_RGB_RENDER_TARGET_header           \
   .yuv_enable = false,  \
   .writeback_buffer = { MALI_RT_BUFFER_header },  \
   .clear = { MALI_RT_CLEAR_header }

#define MALI_RGB_RENDER_TARGET_LENGTH 64
#define MALI_RGB_RENDER_TARGET_ALIGN 64
struct mali_rgb_render_target_packed { uint32_t opaque[16]; };
#define MALI_RGB_RENDER_TARGET_PACKED_T struct mali_rgb_render_target_packed
#define MALI_RGB_RENDER_TARGET_pack(__packed, __unpacked) \
do { \
   MALI_RGB_RENDER_TARGET_PACKED_T __tmp_packed; \
   assert(((__unpacked)->internal_buffer_offset & 0xf) == 0); \
   __tmp_packed.opaque[ 0] = util_bitpack_uint((__unpacked)->internal_buffer_offset >> 4, 4, 15) | \
            util_bitpack_uint((__unpacked)->yuv_enable, 24, 24) | \
            util_bitpack_uint((__unpacked)->dithered_clear, 25, 25) | \
            util_bitpack_uint((__unpacked)->internal_format, 26, 31); \
   __tmp_packed.opaque[ 1] = util_bitpack_uint((__unpacked)->write_enable, 0, 0) | \
            util_bitpack_uint((__unpacked)->writeback_format, 3, 7) | \
            util_bitpack_uint((__unpacked)->writeback_block_format, 8, 11) | \
            util_bitpack_uint((__unpacked)->writeback_msaa, 12, 13) | \
            util_bitpack_uint((__unpacked)->srgb, 14, 14) | \
            util_bitpack_uint((__unpacked)->dithering_enable, 15, 15) | \
            util_bitpack_uint((__unpacked)->swizzle, 16, 27) | \
            util_bitpack_uint((__unpacked)->clean_tile_write_enable, 31, 31); \
   __tmp_packed.opaque[ 2] = 0; \
   __tmp_packed.opaque[ 3] = 0; \
   __tmp_packed.opaque[ 4] = 0; \
   __tmp_packed.opaque[ 5] = 0; \
   __tmp_packed.opaque[ 6] = 0; \
   __tmp_packed.opaque[ 7] = 0; \
   __tmp_packed.opaque[ 8] = util_bitpack_uint((__unpacked)->writeback_buffer.base, 0, 63); \
   __tmp_packed.opaque[ 9] = util_bitpack_uint((__unpacked)->writeback_buffer.base, 0, 63) >> 32; \
   __tmp_packed.opaque[10] = util_bitpack_uint((__unpacked)->writeback_buffer.row_stride, 0, 31); \
   __tmp_packed.opaque[11] = util_bitpack_uint((__unpacked)->writeback_buffer.surface_stride, 0, 31); \
   __tmp_packed.opaque[12] = util_bitpack_uint((__unpacked)->clear.color_0, 0, 31); \
   __tmp_packed.opaque[13] = util_bitpack_uint((__unpacked)->clear.color_1, 0, 31); \
   __tmp_packed.opaque[14] = util_bitpack_uint((__unpacked)->clear.color_2, 0, 31); \
   __tmp_packed.opaque[15] = util_bitpack_uint((__unpacked)->clear.color_3, 0, 31); \
   *(__packed) = __tmp_packed; \
} while (0);



struct MALI_TILER_HEAP {
   uint64_t                             size;
   uint64_t                             base;
   uint64_t                             bottom;
   uint64_t                             top;
};

#define MALI_TILER_HEAP_header                  \
   0

#define MALI_TILER_HEAP_LENGTH 32
#define MALI_TILER_HEAP_ALIGN 64
struct mali_tiler_heap_packed { uint32_t opaque[8]; };
#define MALI_TILER_HEAP_PACKED_T struct mali_tiler_heap_packed
#define MALI_TILER_HEAP_pack(__packed, __unpacked) \
do { \
   MALI_TILER_HEAP_PACKED_T __tmp_packed; \
   __tmp_packed.opaque[ 0] = 0; \
   __tmp_packed.opaque[ 1] = util_bitpack_uint(ALIGN_POT((__unpacked)->size, 4096), 0, 31); \
   __tmp_packed.opaque[ 2] = util_bitpack_uint((__unpacked)->base, 0, 63); \
   __tmp_packed.opaque[ 3] = util_bitpack_uint((__unpacked)->base, 0, 63) >> 32; \
   __tmp_packed.opaque[ 4] = util_bitpack_uint((__unpacked)->bottom, 0, 63); \
   __tmp_packed.opaque[ 5] = util_bitpack_uint((__unpacked)->bottom, 0, 63) >> 32; \
   __tmp_packed.opaque[ 6] = util_bitpack_uint((__unpacked)->top, 0, 63); \
   __tmp_packed.opaque[ 7] = util_bitpack_uint((__unpacked)->top, 0, 63) >> 32; \
   *(__packed) = __tmp_packed; \
} while (0);



struct MALI_TILER_WEIGHTS {
   uint32_t                             weight0;
   uint32_t                             weight1;
   uint32_t                             weight2;
   uint32_t                             weight3;
   uint32_t                             weight4;
   uint32_t                             weight5;
   uint32_t                             weight6;
   uint32_t                             weight7;
};

#define MALI_TILER_WEIGHTS_header               \
   0

#define MALI_TILER_WEIGHTS_LENGTH 32
struct mali_tiler_weights_packed { uint32_t opaque[8]; };
#define MALI_TILER_WEIGHTS_PACKED_T struct mali_tiler_weights_packed
#define MALI_TILER_WEIGHTS_pack(__packed, __unpacked) \
do { \
   MALI_TILER_WEIGHTS_PACKED_T __tmp_packed; \
   __tmp_packed.opaque[ 0] = util_bitpack_uint((__unpacked)->weight0, 16, 31); \
   __tmp_packed.opaque[ 1] = util_bitpack_uint((__unpacked)->weight1, 16, 31); \
   __tmp_packed.opaque[ 2] = util_bitpack_uint((__unpacked)->weight2, 16, 31); \
   __tmp_packed.opaque[ 3] = util_bitpack_uint((__unpacked)->weight3, 16, 31); \
   __tmp_packed.opaque[ 4] = util_bitpack_uint((__unpacked)->weight4, 16, 31); \
   __tmp_packed.opaque[ 5] = util_bitpack_uint((__unpacked)->weight5, 16, 31); \
   __tmp_packed.opaque[ 6] = util_bitpack_uint((__unpacked)->weight6, 16, 31); \
   __tmp_packed.opaque[ 7] = util_bitpack_uint((__unpacked)->weight7, 16, 31); \
   *(__packed) = __tmp_packed; \
} while (0);



struct MALI_TILER_STATE {
   uint64_t                             word0;
   uint64_t                             word1;
   uint64_t                             word2;
   uint64_t                             word3;
   uint64_t                             word4;
   uint64_t                             word5;
   uint64_t                             word6;
   uint64_t                             word7;
   uint64_t                             word8;
   uint64_t                             word9;
   uint64_t                             word10;
   uint64_t                             word11;
   uint64_t                             word12;
   uint64_t                             word13;
   uint64_t                             word14;
   uint64_t                             word15;
};

#define MALI_TILER_STATE_header                 \
   0

#define MALI_TILER_STATE_LENGTH 64
struct mali_tiler_state_packed { uint32_t opaque[16]; };
#define MALI_TILER_STATE_PACKED_T struct mali_tiler_state_packed
#define MALI_TILER_STATE_pack(__packed, __unpacked) \
do { \
   MALI_TILER_STATE_PACKED_T __tmp_packed; \
   __tmp_packed.opaque[ 0] = util_bitpack_uint((__unpacked)->word0, 0, 31); \
   __tmp_packed.opaque[ 1] = util_bitpack_uint((__unpacked)->word1, 0, 31); \
   __tmp_packed.opaque[ 2] = util_bitpack_uint((__unpacked)->word2, 0, 31); \
   __tmp_packed.opaque[ 3] = util_bitpack_uint((__unpacked)->word3, 0, 31); \
   __tmp_packed.opaque[ 4] = util_bitpack_uint((__unpacked)->word4, 0, 31); \
   __tmp_packed.opaque[ 5] = util_bitpack_uint((__unpacked)->word5, 0, 31); \
   __tmp_packed.opaque[ 6] = util_bitpack_uint((__unpacked)->word6, 0, 31); \
   __tmp_packed.opaque[ 7] = util_bitpack_uint((__unpacked)->word7, 0, 31); \
   __tmp_packed.opaque[ 8] = util_bitpack_uint((__unpacked)->word8, 0, 31); \
   __tmp_packed.opaque[ 9] = util_bitpack_uint((__unpacked)->word9, 0, 31); \
   __tmp_packed.opaque[10] = util_bitpack_uint((__unpacked)->word10, 0, 31); \
   __tmp_packed.opaque[11] = util_bitpack_uint((__unpacked)->word11, 0, 31); \
   __tmp_packed.opaque[12] = util_bitpack_uint((__unpacked)->word12, 0, 31); \
   __tmp_packed.opaque[13] = util_bitpack_uint((__unpacked)->word13, 0, 31); \
   __tmp_packed.opaque[14] = util_bitpack_uint((__unpacked)->word14, 0, 31); \
   __tmp_packed.opaque[15] = util_bitpack_uint((__unpacked)->word15, 0, 31); \
   *(__packed) = __tmp_packed; \
} while (0);



struct MALI_TILER_CONTEXT {
   uint64_t                             polygon_list;
   uint32_t                             hierarchy_mask;
   enum mali_sample_pattern             sample_pattern;
   bool                                 update_cost_table;
   uint32_t                             fb_width;
   uint32_t                             fb_height;
   uint64_t                             heap;
   struct MALI_TILER_WEIGHTS            weights;
   struct MALI_TILER_STATE              state;
};

#define MALI_TILER_CONTEXT_header               \
   .weights = { MALI_TILER_WEIGHTS_header },  \
   .state = { MALI_TILER_STATE_header }

#define MALI_TILER_CONTEXT_LENGTH 192
#define MALI_TILER_CONTEXT_ALIGN 64
struct mali_tiler_context_packed { uint32_t opaque[48]; };
#define MALI_TILER_CONTEXT_PACKED_T struct mali_tiler_context_packed
#define MALI_TILER_CONTEXT_pack(__packed, __unpacked) \
do { \
   MALI_TILER_CONTEXT_PACKED_T __tmp_packed; \
   assert((__unpacked)->fb_width >= 1); \
   assert((__unpacked)->fb_height >= 1); \
   __tmp_packed.opaque[ 0] = util_bitpack_uint((__unpacked)->polygon_list, 0, 63); \
   __tmp_packed.opaque[ 1] = util_bitpack_uint((__unpacked)->polygon_list, 0, 63) >> 32; \
   __tmp_packed.opaque[ 2] = util_bitpack_uint((__unpacked)->hierarchy_mask, 0, 12) | \
            util_bitpack_uint((__unpacked)->sample_pattern, 13, 15) | \
            util_bitpack_uint((__unpacked)->update_cost_table, 16, 16); \
   __tmp_packed.opaque[ 3] = util_bitpack_uint((__unpacked)->fb_width - 1, 0, 15) | \
            util_bitpack_uint((__unpacked)->fb_height - 1, 16, 31); \
   __tmp_packed.opaque[ 4] = 0; \
   __tmp_packed.opaque[ 5] = 0; \
   __tmp_packed.opaque[ 6] = util_bitpack_uint((__unpacked)->heap, 0, 63); \
   __tmp_packed.opaque[ 7] = util_bitpack_uint((__unpacked)->heap, 0, 63) >> 32; \
   __tmp_packed.opaque[ 8] = util_bitpack_uint((__unpacked)->weights.weight0, 16, 31); \
   __tmp_packed.opaque[ 9] = util_bitpack_uint((__unpacked)->weights.weight1, 16, 31); \
   __tmp_packed.opaque[10] = util_bitpack_uint((__unpacked)->weights.weight2, 16, 31); \
   __tmp_packed.opaque[11] = util_bitpack_uint((__unpacked)->weights.weight3, 16, 31); \
   __tmp_packed.opaque[12] = util_bitpack_uint((__unpacked)->weights.weight4, 16, 31); \
   __tmp_packed.opaque[13] = util_bitpack_uint((__unpacked)->weights.weight5, 16, 31); \
   __tmp_packed.opaque[14] = util_bitpack_uint((__unpacked)->weights.weight6, 16, 31); \
   __tmp_packed.opaque[15] = util_bitpack_uint((__unpacked)->weights.weight7, 16, 31); \
   __tmp_packed.opaque[16] = 0; \
   __tmp_packed.opaque[17] = 0; \
   __tmp_packed.opaque[18] = 0; \
   __tmp_packed.opaque[19] = 0; \
   __tmp_packed.opaque[20] = 0; \
   __tmp_packed.opaque[21] = 0; \
   __tmp_packed.opaque[22] = 0; \
   __tmp_packed.opaque[23] = 0; \
   __tmp_packed.opaque[24] = 0; \
   __tmp_packed.opaque[25] = 0; \
   __tmp_packed.opaque[26] = 0; \
   __tmp_packed.opaque[27] = 0; \
   __tmp_packed.opaque[28] = 0; \
   __tmp_packed.opaque[29] = 0; \
   __tmp_packed.opaque[30] = 0; \
   __tmp_packed.opaque[31] = 0; \
   __tmp_packed.opaque[32] = util_bitpack_uint((__unpacked)->state.word0, 0, 31); \
   __tmp_packed.opaque[33] = util_bitpack_uint((__unpacked)->state.word1, 0, 31); \
   __tmp_packed.opaque[34] = util_bitpack_uint((__unpacked)->state.word2, 0, 31); \
   __tmp_packed.opaque[35] = util_bitpack_uint((__unpacked)->state.word3, 0, 31); \
   __tmp_packed.opaque[36] = util_bitpack_uint((__unpacked)->state.word4, 0, 31); \
   __tmp_packed.opaque[37] = util_bitpack_uint((__unpacked)->state.word5, 0, 31); \
   __tmp_packed.opaque[38] = util_bitpack_uint((__unpacked)->state.word6, 0, 31); \
   __tmp_packed.opaque[39] = util_bitpack_uint((__unpacked)->state.word7, 0, 31); \
   __tmp_packed.opaque[40] = util_bitpack_uint((__unpacked)->state.word8, 0, 31); \
   __tmp_packed.opaque[41] = util_bitpack_uint((__unpacked)->state.word9, 0, 31); \
   __tmp_packed.opaque[42] = util_bitpack_uint((__unpacked)->state.word10, 0, 31); \
   __tmp_packed.opaque[43] = util_bitpack_uint((__unpacked)->state.word11, 0, 31); \
   __tmp_packed.opaque[44] = util_bitpack_uint((__unpacked)->state.word12, 0, 31); \
   __tmp_packed.opaque[45] = util_bitpack_uint((__unpacked)->state.word13, 0, 31); \
   __tmp_packed.opaque[46] = util_bitpack_uint((__unpacked)->state.word14, 0, 31); \
   __tmp_packed.opaque[47] = util_bitpack_uint((__unpacked)->state.word15, 0, 31); \
   *(__packed) = __tmp_packed; \
} while (0);



struct MALI_FRAGMENT_JOB_PAYLOAD {
   uint32_t                             bound_min_x;
   uint32_t                             bound_min_y;
   uint32_t                             bound_max_x;
   uint32_t                             bound_max_y;
   bool                                 has_tile_enable_map;
   uint64_t                             framebuffer;
   uint64_t                             tile_enable_map;
   uint32_t                             tile_enable_map_row_stride;
};

#define MALI_FRAGMENT_JOB_PAYLOAD_header        \
   0

#define MALI_FRAGMENT_JOB_PAYLOAD_LENGTH 32
struct mali_fragment_job_payload_packed { uint32_t opaque[8]; };
#define MALI_FRAGMENT_JOB_PAYLOAD_PACKED_T struct mali_fragment_job_payload_packed
#define MALI_FRAGMENT_JOB_PAYLOAD_pack(__packed, __unpacked) \
do { \
   MALI_FRAGMENT_JOB_PAYLOAD_PACKED_T __tmp_packed; \
   __tmp_packed.opaque[ 0] = util_bitpack_uint((__unpacked)->bound_min_x, 0, 11) | \
            util_bitpack_uint((__unpacked)->bound_min_y, 16, 27); \
   __tmp_packed.opaque[ 1] = util_bitpack_uint((__unpacked)->bound_max_x, 0, 11) | \
            util_bitpack_uint((__unpacked)->bound_max_y, 16, 27) | \
            util_bitpack_uint((__unpacked)->has_tile_enable_map, 31, 31); \
   __tmp_packed.opaque[ 2] = util_bitpack_uint((__unpacked)->framebuffer, 0, 63); \
   __tmp_packed.opaque[ 3] = util_bitpack_uint((__unpacked)->framebuffer, 0, 63) >> 32; \
   __tmp_packed.opaque[ 4] = util_bitpack_uint((__unpacked)->tile_enable_map, 0, 63); \
   __tmp_packed.opaque[ 5] = util_bitpack_uint((__unpacked)->tile_enable_map, 0, 63) >> 32; \
   __tmp_packed.opaque[ 6] = util_bitpack_uint((__unpacked)->tile_enable_map_row_stride, 0, 7); \
   __tmp_packed.opaque[ 7] = 0; \
   *(__packed) = __tmp_packed; \
} while (0);


}
