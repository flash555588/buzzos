#ifndef BUZZOS_VIRGL_PROTOCOL_H
#define BUZZOS_VIRGL_PROTOCOL_H

/*
 * Trimmed virgl command-stream constants.
 *
 * Source: virglrenderer src/virgl_protocol.h, Copyright 2014, 2015 Red Hat,
 * MIT licensed.  Only the subset BuzzOS encodes is reproduced here so the
 * kernel stays freestanding and does not depend on host headers.  Values must
 * match the host virglrenderer exactly -- do not "clean up" the numbers.
 *
 * Command stream layout: a flat array of dwords.  Every packet starts with
 *   header = cmd | (object_type << 8) | (payload_dwords << 16)
 * where payload_dwords counts the dwords AFTER the header.
 */

#include <stdint.h>

#define VIRGL_CMD0(cmd, obj, len) \
    ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))

enum virgl_object_type {
    VIRGL_OBJECT_NULL = 0,
    VIRGL_OBJECT_BLEND = 1,
    VIRGL_OBJECT_RASTERIZER = 2,
    VIRGL_OBJECT_DSA = 3,
    VIRGL_OBJECT_SHADER = 4,
    VIRGL_OBJECT_VERTEX_ELEMENTS = 5,
    VIRGL_OBJECT_SAMPLER_VIEW = 6,
    VIRGL_OBJECT_SAMPLER_STATE = 7,
    VIRGL_OBJECT_SURFACE = 8,
    VIRGL_OBJECT_QUERY = 9,
    VIRGL_OBJECT_STREAMOUT_TARGET = 10,
    VIRGL_OBJECT_MSAA_SURFACE = 11,
};

enum virgl_context_cmd {
    VIRGL_CCMD_NOP = 0,
    VIRGL_CCMD_CREATE_OBJECT = 1,
    VIRGL_CCMD_BIND_OBJECT = 2,
    VIRGL_CCMD_DESTROY_OBJECT = 3,
    VIRGL_CCMD_SET_VIEWPORT_STATE = 4,
    VIRGL_CCMD_SET_FRAMEBUFFER_STATE = 5,
    VIRGL_CCMD_SET_VERTEX_BUFFERS = 6,
    VIRGL_CCMD_CLEAR = 7,
    VIRGL_CCMD_DRAW_VBO = 8,
    VIRGL_CCMD_RESOURCE_INLINE_WRITE = 9,
    VIRGL_CCMD_SET_SAMPLER_VIEWS = 10,
    VIRGL_CCMD_SET_INDEX_BUFFER = 11,
    VIRGL_CCMD_SET_CONSTANT_BUFFER = 12,
    VIRGL_CCMD_SET_STENCIL_REF = 13,
    VIRGL_CCMD_SET_BLEND_COLOR = 14,
    VIRGL_CCMD_SET_SCISSOR_STATE = 15,
    VIRGL_CCMD_BLIT = 16,
    VIRGL_CCMD_RESOURCE_COPY_REGION = 17,
    VIRGL_CCMD_BIND_SAMPLER_STATES = 18,
    VIRGL_CCMD_SET_SUB_CTX = 28,
    VIRGL_CCMD_CREATE_SUB_CTX = 29,
    VIRGL_CCMD_DESTROY_SUB_CTX = 30,
    VIRGL_CCMD_BIND_SHADER = 31,
};

/* Shader stage selectors (PIPE_SHADER_*). */
enum virgl_shader_type {
    VIRGL_SHADER_VERTEX = 0,
    VIRGL_SHADER_FRAGMENT = 1,
    VIRGL_SHADER_GEOMETRY = 2,
};

/* Clear buffer mask (PIPE_CLEAR_*). */
enum {
    VIRGL_CLEAR_DEPTH = 1u << 0,
    VIRGL_CLEAR_STENCIL = 1u << 1,
    VIRGL_CLEAR_COLOR0 = 1u << 2,
};

/* Primitive modes (PIPE_PRIM_*). */
enum {
    VIRGL_PRIM_TRIANGLES = 4,
    VIRGL_PRIM_TRIANGLE_STRIP = 5,
};

/* --- packet payload sizes / slot offsets (dword indices after header) --- */

#define VIRGL_OBJ_SURFACE_SIZE 5
#define VIRGL_OBJ_SAMPLER_VIEW_SIZE 6
#define VIRGL_OBJ_SAMPLER_STATE_SIZE 9
#define VIRGL_OBJ_DSA_SIZE 5
#define VIRGL_OBJ_RS_SIZE 9
#define VIRGL_OBJ_BLEND_SIZE (8 + 3) /* VIRGL_MAX_COLOR_BUFS + 3 */
#define VIRGL_OBJ_VERTEX_ELEMENTS_SIZE(n) (((n) * 4) + 1)
#define VIRGL_OBJ_CLEAR_SIZE 8
#define VIRGL_SET_FRAMEBUFFER_STATE_SIZE(nr_cbufs) ((nr_cbufs) + 2)
#define VIRGL_SET_VIEWPORT_STATE_SIZE(n) ((6 * (n)) + 1)
#define VIRGL_SET_VERTEX_BUFFERS_SIZE(n) ((n) * 3)
#define VIRGL_SET_SAMPLER_VIEWS_SIZE(n) ((n) + 2)
#define VIRGL_BIND_SAMPLER_STATES_SIZE(n) ((n) + 2)
#define VIRGL_DRAW_VBO_SIZE 12
#define VIRGL_BIND_SHADER_SIZE 2
#define VIRGL_SET_SCISSOR_STATE_SIZE(n) (1 + 2 * (n))

/* Blend state bit packing (dword S2, one per render target). */
#define VIRGL_OBJ_BLEND_S2_RT_BLEND_ENABLE(x) (((x) & 0x1) << 0)
#define VIRGL_OBJ_BLEND_S2_RT_RGB_FUNC(x) (((x) & 0x7) << 1)
#define VIRGL_OBJ_BLEND_S2_RT_RGB_SRC_FACTOR(x) (((x) & 0x1f) << 4)
#define VIRGL_OBJ_BLEND_S2_RT_RGB_DST_FACTOR(x) (((x) & 0x1f) << 9)
#define VIRGL_OBJ_BLEND_S2_RT_ALPHA_FUNC(x) (((x) & 0x7) << 14)
#define VIRGL_OBJ_BLEND_S2_RT_ALPHA_SRC_FACTOR(x) (((x) & 0x1f) << 17)
#define VIRGL_OBJ_BLEND_S2_RT_ALPHA_DST_FACTOR(x) (((x) & 0x1f) << 22)
#define VIRGL_OBJ_BLEND_S2_RT_COLORMASK(x) (((x) & 0xf) << 27)

/* Rasterizer state dword S0 bits actually used by the compositor. */
#define VIRGL_OBJ_RS_S0_DEPTH_CLIP(x) (((x) & 0x1) << 1)
#define VIRGL_OBJ_RS_S0_CULL_FACE(x) (((x) & 0x3) << 8)
#define VIRGL_OBJ_RS_S0_FILL_FRONT(x) (((x) & 0x3) << 10)
#define VIRGL_OBJ_RS_S0_FILL_BACK(x) (((x) & 0x3) << 12)
#define VIRGL_OBJ_RS_S0_SCISSOR(x) (((x) & 0x1) << 14)
#define VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(x) (((x) & 0x1) << 29)
#define VIRGL_OBJ_RS_S0_BOTTOM_EDGE_RULE(x) (((x) & 0x1) << 30)

/* Sampler state dword S0 bits. */
#define VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_S(x) (((x) & 0x7) << 0)
#define VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_T(x) (((x) & 0x7) << 3)
#define VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_R(x) (((x) & 0x7) << 6)
#define VIRGL_OBJ_SAMPLE_STATE_S0_MIN_IMG_FILTER(x) (((x) & 0x3) << 9)
#define VIRGL_OBJ_SAMPLE_STATE_S0_MIN_MIP_FILTER(x) (((x) & 0x3) << 11)
#define VIRGL_OBJ_SAMPLE_STATE_S0_MAG_IMG_FILTER(x) (((x) & 0x3) << 13)

/* Sampler view swizzle (identity = R,G,B,A = 0,1,2,3). */
#define VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_R(x) (((x) & 0x7) << 0)
#define VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_G(x) (((x) & 0x7) << 3)
#define VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_B(x) (((x) & 0x7) << 6)
#define VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_A(x) (((x) & 0x7) << 9)

/* Shader object header slots. */
#define VIRGL_OBJ_SHADER_OFFSET_CONT (0x1u << 31)

/* PIPE_BLENDFACTOR_* (Mesa src/util/blend.h).  Counts up from 1; the inverted
 * half is the base value OR'd with PIPE_BLENDFACTOR_INVERT_BIT (0x10).  So
 * SRC_ALPHA is 3 and INV_SRC_ALPHA is 0x13 -- transposing these silently
 * selects DST_ALPHA/INV_DST_ALPHA, which against an opaque target collapses
 * to src*1 + dst*0 and renders fully opaque. */
enum {
    VIRGL_BLENDFACTOR_ONE = 0x01,
    VIRGL_BLENDFACTOR_SRC_ALPHA = 0x03,
    VIRGL_BLENDFACTOR_DST_ALPHA = 0x04,
    VIRGL_BLENDFACTOR_ZERO = 0x11,
    VIRGL_BLENDFACTOR_INV_SRC_ALPHA = 0x13,
    VIRGL_BLENDFACTOR_INV_DST_ALPHA = 0x14,
};

/* PIPE_BLEND_* equation. */
enum { VIRGL_BLEND_ADD = 0 };

/* PIPE_TEX_FILTER_* */
enum {
    VIRGL_TEX_FILTER_NEAREST = 0,
    VIRGL_TEX_FILTER_LINEAR = 1,
};

/* PIPE_TEX_MIPFILTER_NONE */
enum { VIRGL_TEX_MIPFILTER_NONE = 2 };

/* PIPE_TEX_WRAP_CLAMP_TO_EDGE */
enum { VIRGL_TEX_WRAP_CLAMP_TO_EDGE = 2 };

/* PIPE_POLYGON_MODE_FILL / PIPE_FACE_NONE */
enum { VIRGL_POLYGON_MODE_FILL = 0, VIRGL_FACE_NONE = 0 };

/* --- from virglrenderer src/virgl_hw.h (MIT) --- */

/* Resource bind flags. */
enum {
    VIRGL_BIND_DEPTH_STENCIL = 1u << 0,
    VIRGL_BIND_RENDER_TARGET = 1u << 1,
    VIRGL_BIND_SAMPLER_VIEW = 1u << 3,
    VIRGL_BIND_VERTEX_BUFFER = 1u << 4,
    VIRGL_BIND_INDEX_BUFFER = 1u << 5,
    VIRGL_BIND_CONSTANT_BUFFER = 1u << 6,
    VIRGL_BIND_DISPLAY_TARGET = 1u << 7,
    VIRGL_BIND_SCANOUT = 1u << 18,
    VIRGL_BIND_LINEAR = 1u << 22,
};

/* Pixel formats.  These share numbering with the virtio-gpu 2D format enum
 * for the BGRA/BGRX entries, which is why the existing 2D scanout constant
 * (B8G8R8X8_UNORM = 2) lines up. */
enum {
    VIRGL_FORMAT_NONE = 0,
    VIRGL_FORMAT_B8G8R8A8_UNORM = 1,
    VIRGL_FORMAT_B8G8R8X8_UNORM = 2,
    VIRGL_FORMAT_R32G32_FLOAT = 29,
    VIRGL_FORMAT_R32G32B32A32_FLOAT = 31,
    VIRGL_FORMAT_R8G8B8A8_UNORM = 67,
};

/* IEEE-754 binary32 bit patterns for the constants the kernel-side encoder
 * needs.  The kernel is built with -mgeneral-regs-only and must not touch the
 * FPU/SIMD register file, so float literals are spelled as bits. */
enum {
    VIRGL_F32_NEG_1 = 0xBF800000u,
    VIRGL_F32_0 = 0x00000000u,
    VIRGL_F32_1 = 0x3F800000u,
};

/* Shader stage encoding on the wire (virgl_shader_stage_convert): the
 * fragment stage is 1 and vertex is 0, matching PIPE_SHADER_*. */

/* CREATE_OBJECT(SHADER) payload length, no stream-output. */
#define VIRGL_OBJ_SHADER_HDR_SIZE(nso) (5 + ((nso) ? (2 * (nso)) + 4 : 0))
#define VIRGL_OBJ_SHADER_OFFSET_VAL(x) (((x) & 0x7fffffffu) << 0)

/* PIPE_PRIM_* / MESA_PRIM_* triangle strip. */

#endif /* BUZZOS_VIRGL_PROTOCOL_H */
