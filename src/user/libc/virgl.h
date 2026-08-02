#ifndef BUZZOS_VIRGL_H
#define BUZZOS_VIRGL_H

/*
 * User-space virgl command encoder.
 *
 * The kernel owns the GPU context and resources but deliberately does not
 * encode command streams: TGSI shaders are text and vertex data is floating
 * point, neither of which belongs in a -mno-sse freestanding kernel.  This
 * header builds the dword stream and hands it to gpu3d_submit().
 *
 * Packet layout:  cmd | (object_type << 8) | (payload_dwords << 16),
 * followed by the payload.  Constants mirror virglrenderer's virgl_protocol.h
 * and virgl_hw.h (MIT); see src/kernel/drv/virgl_protocol.h for the vendored
 * kernel-side copy.
 *
 * ALPHA CONVENTION -- read before uploading any texture.  The rest of BuzzOS
 * stores opaque pixels as 0x00RRGGBB, i.e. the alpha byte is *zero*.  Sampled
 * as B8G8R8A8 with SRC_ALPHA blending that means every texel is fully
 * transparent and nothing appears on screen.  Surfaces destined for the GPU
 * must set alpha to 0xFF (or be uploaded as B8G8R8X8_UNORM, which forces
 * alpha to 1.0 and ignores the stored byte).
 *
 * DEBUGGING -- a malformed command stream does not fail loudly: SUBMIT_3D
 * still returns OK and the frame simply comes out wrong or empty.  Verify
 * every protocol constant against the real headers rather than from memory.
 */

#include <stdint.h>
#include <string.h>
#include "libc.h"

#define VIRGL_CMD0(cmd, obj, len) \
    ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))

enum {
    VIRGL_OBJECT_NULL = 0,
    VIRGL_OBJECT_BLEND = 1,
    VIRGL_OBJECT_RASTERIZER = 2,
    VIRGL_OBJECT_DSA = 3,
    VIRGL_OBJECT_SHADER = 4,
    VIRGL_OBJECT_VERTEX_ELEMENTS = 5,
    VIRGL_OBJECT_SAMPLER_VIEW = 6,
    VIRGL_OBJECT_SAMPLER_STATE = 7,
    VIRGL_OBJECT_SURFACE = 8,
};

enum {
    VIRGL_CCMD_CREATE_OBJECT = 1,
    VIRGL_CCMD_BIND_OBJECT = 2,
    VIRGL_CCMD_DESTROY_OBJECT = 3,
    VIRGL_CCMD_SET_VIEWPORT_STATE = 4,
    VIRGL_CCMD_SET_FRAMEBUFFER_STATE = 5,
    VIRGL_CCMD_SET_VERTEX_BUFFERS = 6,
    VIRGL_CCMD_CLEAR = 7,
    VIRGL_CCMD_DRAW_VBO = 8,
    VIRGL_CCMD_SET_SAMPLER_VIEWS = 10,
    VIRGL_CCMD_SET_CONSTANT_BUFFER = 12,
    VIRGL_CCMD_BIND_SAMPLER_STATES = 18,
    VIRGL_CCMD_BIND_SHADER = 31,
};

/* Shader stages, and the resource targets we use. */
enum { VIRGL_SHADER_VERTEX = 0, VIRGL_SHADER_FRAGMENT = 1 };
enum { VIRGL_TARGET_BUFFER = 0, VIRGL_TARGET_TEXTURE_2D = 2 };

enum {
    VIRGL_BIND_RENDER_TARGET = 1u << 1,
    VIRGL_BIND_SAMPLER_VIEW = 1u << 3,
    VIRGL_BIND_VERTEX_BUFFER = 1u << 4,
    VIRGL_BIND_CONSTANT_BUFFER = 1u << 6,
    VIRGL_BIND_SCANOUT = 1u << 18,
};

enum {
    VIRGL_FORMAT_B8G8R8A8_UNORM = 1,
    VIRGL_FORMAT_B8G8R8X8_UNORM = 2,
    VIRGL_FORMAT_R32G32_FLOAT = 29,
    VIRGL_FORMAT_R32G32B32A32_FLOAT = 31,
    VIRGL_FORMAT_R8_UNORM = 64,
    VIRGL_FORMAT_R8G8B8A8_UNORM = 67,
};

enum { VIRGL_CLEAR_COLOR0 = 1u << 2 };
enum { VIRGL_PRIM_TRIANGLE_STRIP = 5 };

/* PIPE_BLENDFACTOR_* (Mesa src/util/blend.h).  The enum counts up from 1 and
 * the "inverted" half is the same value OR'd with PIPE_BLENDFACTOR_INVERT_BIT
 * (0x10) -- so SRC_ALPHA is 3, not 4, and INV_SRC_ALPHA is 0x13, not 0x14.
 * Getting these wrong silently yields DST_ALPHA/INV_DST_ALPHA, which against
 * an opaque render target collapses to src*1 + dst*0: fully opaque output
 * with blending nominally enabled. */
enum {
    VIRGL_BLENDFACTOR_ONE = 0x01,
    VIRGL_BLENDFACTOR_SRC_COLOR = 0x02,
    VIRGL_BLENDFACTOR_SRC_ALPHA = 0x03,
    VIRGL_BLENDFACTOR_DST_ALPHA = 0x04,
    VIRGL_BLENDFACTOR_DST_COLOR = 0x05,
    VIRGL_BLENDFACTOR_ZERO = 0x11,
    VIRGL_BLENDFACTOR_INV_SRC_COLOR = 0x12,
    VIRGL_BLENDFACTOR_INV_SRC_ALPHA = 0x13,
    VIRGL_BLENDFACTOR_INV_DST_ALPHA = 0x14,
    VIRGL_BLENDFACTOR_INV_DST_COLOR = 0x15,
    VIRGL_BLEND_ADD = 0,
};

enum {
    VIRGL_TEX_FILTER_NEAREST = 0,
    VIRGL_TEX_FILTER_LINEAR = 1,
    VIRGL_TEX_MIPFILTER_NONE = 2,
    VIRGL_TEX_WRAP_CLAMP_TO_EDGE = 2,
};

/* Bit packing for the state objects we build. */
#define VIRGL_OBJ_BLEND_S2_RT_BLEND_ENABLE(x) (((x) & 0x1) << 0)
#define VIRGL_OBJ_BLEND_S2_RT_RGB_FUNC(x) (((x) & 0x7) << 1)
#define VIRGL_OBJ_BLEND_S2_RT_RGB_SRC_FACTOR(x) (((x) & 0x1f) << 4)
#define VIRGL_OBJ_BLEND_S2_RT_RGB_DST_FACTOR(x) (((x) & 0x1f) << 9)
#define VIRGL_OBJ_BLEND_S2_RT_ALPHA_FUNC(x) (((x) & 0x7) << 14)
#define VIRGL_OBJ_BLEND_S2_RT_ALPHA_SRC_FACTOR(x) (((x) & 0x1f) << 17)
#define VIRGL_OBJ_BLEND_S2_RT_ALPHA_DST_FACTOR(x) (((x) & 0x1f) << 22)
#define VIRGL_OBJ_BLEND_S2_RT_COLORMASK(x) (((x) & 0xf) << 27)

#define VIRGL_OBJ_RS_S0_DEPTH_CLIP(x) (((x) & 0x1) << 1)
#define VIRGL_OBJ_RS_S0_CULL_FACE(x) (((x) & 0x3) << 8)
#define VIRGL_OBJ_RS_S0_FILL_FRONT(x) (((x) & 0x3) << 10)
#define VIRGL_OBJ_RS_S0_FILL_BACK(x) (((x) & 0x3) << 12)
#define VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(x) (((x) & 0x1) << 29)
#define VIRGL_OBJ_RS_S0_BOTTOM_EDGE_RULE(x) (((x) & 0x1) << 30)

#define VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_S(x) (((x) & 0x7) << 0)
#define VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_T(x) (((x) & 0x7) << 3)
#define VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_R(x) (((x) & 0x7) << 6)
#define VIRGL_OBJ_SAMPLE_STATE_S0_MIN_IMG_FILTER(x) (((x) & 0x3) << 9)
#define VIRGL_OBJ_SAMPLE_STATE_S0_MIN_MIP_FILTER(x) (((x) & 0x3) << 11)
#define VIRGL_OBJ_SAMPLE_STATE_S0_MAG_IMG_FILTER(x) (((x) & 0x3) << 13)

#define VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_R(x) (((x) & 0x7) << 0)
#define VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_G(x) (((x) & 0x7) << 3)
#define VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_B(x) (((x) & 0x7) << 6)
#define VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_A(x) (((x) & 0x7) << 9)

#define VIRGL_OBJ_SHADER_OFFSET_VAL(x) (((x) & 0x7fffffffu) << 0)
#define VIRGL_OBJ_SHADER_OFFSET_CONT (0x1u << 31)

/* Object handles are context-local and allocated by the client. */
enum {
    VIRGL_H_NONE = 0,
    VIRGL_H_SURFACE = 1,
    VIRGL_H_BLEND = 2,
    VIRGL_H_RASTERIZER = 3,
    VIRGL_H_DSA = 4,
    VIRGL_H_VS = 5,
    VIRGL_H_FS = 6,
    VIRGL_H_FS_SOLID = 9,
    VIRGL_H_VERTEX_ELEMENTS = 7,
    VIRGL_H_SAMPLER_STATE = 8,
    VIRGL_H_SAMPLER_VIEW_BASE = 16, /* one per texture */
};

#define VIRGL_CMD_CAPACITY 4096

struct virgl_cmdbuf {
    uint32_t dw[VIRGL_CMD_CAPACITY];
    uint32_t len;
    int overflow;
};

static inline void virgl_reset(struct virgl_cmdbuf *b) {
    b->len = 0;
    b->overflow = 0;
}

static inline void virgl_put(struct virgl_cmdbuf *b, uint32_t value) {
    if (b->len >= VIRGL_CMD_CAPACITY) {
        b->overflow = 1;
        return;
    }
    b->dw[b->len++] = value;
}

static inline void virgl_packet(struct virgl_cmdbuf *b, uint32_t cmd,
                                uint32_t object, uint32_t payload) {
    virgl_put(b, VIRGL_CMD0(cmd, object, payload));
}

/* Reinterpret a float as its IEEE-754 bits for the wire. */
static inline uint32_t virgl_f32(float value) {
    union { float f; uint32_t u; } bits;
    bits.f = value;
    return bits.u;
}

static inline int virgl_flush(struct virgl_cmdbuf *b) {
    int result;
    if (b->overflow || !b->len)
        return -1;
    result = gpu3d_submit(b->dw, b->len);
    virgl_reset(b);
    return result;
}

/* ---- object creation ---- */

static inline void virgl_create_surface(struct virgl_cmdbuf *b,
                                        uint32_t handle, uint32_t resource,
                                        uint32_t format) {
    virgl_packet(b, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    virgl_put(b, handle);
    virgl_put(b, resource);
    virgl_put(b, format);
    virgl_put(b, 0); /* texture level */
    virgl_put(b, 0); /* first_layer | (last_layer << 16) */
}

/* Straight (non-premultiplied) alpha over the destination. */
static inline void virgl_create_blend(struct virgl_cmdbuf *b, uint32_t handle,
                                      int blend_enable) {
    uint32_t rt0 =
        VIRGL_OBJ_BLEND_S2_RT_BLEND_ENABLE(blend_enable ? 1 : 0) |
        VIRGL_OBJ_BLEND_S2_RT_RGB_FUNC(VIRGL_BLEND_ADD) |
        VIRGL_OBJ_BLEND_S2_RT_RGB_SRC_FACTOR(VIRGL_BLENDFACTOR_SRC_ALPHA) |
        VIRGL_OBJ_BLEND_S2_RT_RGB_DST_FACTOR(VIRGL_BLENDFACTOR_INV_SRC_ALPHA) |
        VIRGL_OBJ_BLEND_S2_RT_ALPHA_FUNC(VIRGL_BLEND_ADD) |
        VIRGL_OBJ_BLEND_S2_RT_ALPHA_SRC_FACTOR(VIRGL_BLENDFACTOR_ONE) |
        VIRGL_OBJ_BLEND_S2_RT_ALPHA_DST_FACTOR(VIRGL_BLENDFACTOR_INV_SRC_ALPHA) |
        VIRGL_OBJ_BLEND_S2_RT_COLORMASK(0xF);
    virgl_packet(b, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11);
    virgl_put(b, handle);
    virgl_put(b, 0); /* S0: no independent blend, no logicop, no dither */
    virgl_put(b, 0); /* S1: logicop func */
    virgl_put(b, rt0);
    for (int i = 1; i < 8; i++)
        virgl_put(b, 0);
}

static inline void virgl_create_rasterizer(struct virgl_cmdbuf *b,
                                           uint32_t handle) {
    uint32_t s0 = VIRGL_OBJ_RS_S0_DEPTH_CLIP(1) |
                  VIRGL_OBJ_RS_S0_CULL_FACE(0) |   /* PIPE_FACE_NONE */
                  VIRGL_OBJ_RS_S0_FILL_FRONT(0) |  /* FILL */
                  VIRGL_OBJ_RS_S0_FILL_BACK(0) |
                  VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1) |
                  VIRGL_OBJ_RS_S0_BOTTOM_EDGE_RULE(1);
    virgl_packet(b, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 9);
    virgl_put(b, handle);
    virgl_put(b, s0);
    virgl_put(b, virgl_f32(1.0f)); /* point size */
    virgl_put(b, 0);               /* sprite coord enable */
    virgl_put(b, 0);               /* S3: stipple / clip planes */
    virgl_put(b, virgl_f32(1.0f)); /* line width */
    virgl_put(b, virgl_f32(0.0f)); /* offset units */
    virgl_put(b, virgl_f32(0.0f)); /* offset scale */
    virgl_put(b, virgl_f32(0.0f)); /* offset clamp */
}

/* Depth and stencil fully disabled: the compositor draws back-to-front. */
static inline void virgl_create_dsa(struct virgl_cmdbuf *b, uint32_t handle) {
    virgl_packet(b, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5);
    virgl_put(b, handle);
    virgl_put(b, 0); /* S0: depth/alpha disabled */
    virgl_put(b, 0); /* S1: stencil front */
    virgl_put(b, 0); /* S2: stencil back */
    virgl_put(b, virgl_f32(0.0f)); /* alpha ref */
}

static inline void virgl_create_sampler_state(struct virgl_cmdbuf *b,
                                              uint32_t handle, int linear) {
    int filter = linear ? VIRGL_TEX_FILTER_LINEAR : VIRGL_TEX_FILTER_NEAREST;
    uint32_t s0 =
        VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_S(VIRGL_TEX_WRAP_CLAMP_TO_EDGE) |
        VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_T(VIRGL_TEX_WRAP_CLAMP_TO_EDGE) |
        VIRGL_OBJ_SAMPLE_STATE_S0_WRAP_R(VIRGL_TEX_WRAP_CLAMP_TO_EDGE) |
        VIRGL_OBJ_SAMPLE_STATE_S0_MIN_IMG_FILTER(filter) |
        VIRGL_OBJ_SAMPLE_STATE_S0_MIN_MIP_FILTER(VIRGL_TEX_MIPFILTER_NONE) |
        VIRGL_OBJ_SAMPLE_STATE_S0_MAG_IMG_FILTER(filter);
    virgl_packet(b, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_STATE, 9);
    virgl_put(b, handle);
    virgl_put(b, s0);
    virgl_put(b, virgl_f32(0.0f)); /* lod bias */
    virgl_put(b, virgl_f32(0.0f)); /* min lod */
    virgl_put(b, virgl_f32(0.0f)); /* max lod */
    for (int i = 0; i < 4; i++)
        virgl_put(b, 0); /* border colour */
}

static inline void virgl_create_sampler_view(struct virgl_cmdbuf *b,
                                             uint32_t handle,
                                             uint32_t resource,
                                             uint32_t format) {
    /* The format dword carries the texture target in its high byte. */
    uint32_t format_target = format | ((uint32_t)VIRGL_TARGET_TEXTURE_2D << 24);
    uint32_t swizzle = VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_R(0) |
                       VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_G(1) |
                       VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_B(2) |
                       VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE_A(3);
    virgl_packet(b, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SAMPLER_VIEW, 6);
    virgl_put(b, handle);
    virgl_put(b, resource);
    virgl_put(b, format_target);
    virgl_put(b, 0); /* first layer */
    virgl_put(b, 0); /* last level */
    virgl_put(b, swizzle);
}

/* One vertex element set: vec2 position + vec2 texcoord, interleaved. */
static inline void virgl_create_vertex_elements(struct virgl_cmdbuf *b,
                                                uint32_t handle) {
    virgl_packet(b, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS,
                 (2 * 4) + 1);
    virgl_put(b, handle);
    virgl_put(b, 0); /* src_offset */
    virgl_put(b, 0); /* instance divisor */
    virgl_put(b, 0); /* vertex buffer index */
    virgl_put(b, VIRGL_FORMAT_R32G32_FLOAT);
    virgl_put(b, 8); /* src_offset */
    virgl_put(b, 0);
    virgl_put(b, 0);
    virgl_put(b, VIRGL_FORMAT_R32G32_FLOAT);
}

/* TGSI shader text.  virglrenderer parses this and emits GLSL, so the guest
 * never links a shader compiler.  Length includes the NUL terminator.
 *
 * num_tokens sizes the host's token array before it parses.  Under-counting
 * makes the parse fail *silently* -- SUBMIT_3D still returns OK and nothing
 * renders -- so it is derived generously from the text length rather than
 * guessed.  Mesa counts real tokens; one per 4 bytes of text is a safe
 * over-estimate for the small shaders the compositor uses. */
static inline void virgl_create_shader(struct virgl_cmdbuf *b, uint32_t handle,
                                       uint32_t stage, const char *tgsi) {
    uint32_t bytes = (uint32_t)strlen(tgsi) + 1u;
    uint32_t words = (bytes + 3u) / 4u;
    uint32_t payload = 5u + words;
    uint32_t num_tokens = words + 32u;
    uint32_t i;

    virgl_packet(b, VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, payload);
    virgl_put(b, handle);
    virgl_put(b, stage);
    virgl_put(b, VIRGL_OBJ_SHADER_OFFSET_VAL(bytes)); /* single pass */
    virgl_put(b, num_tokens);
    virgl_put(b, 0); /* stream-output outputs */
    for (i = 0; i < words; i++) {
        uint32_t value = 0;
        for (uint32_t byte = 0; byte < 4u; byte++) {
            uint32_t index = i * 4u + byte;
            uint8_t c = index < bytes ? (uint8_t)tgsi[index] : 0u;
            if (index == bytes - 1u)
                c = 0u;
            value |= (uint32_t)c << (byte * 8);
        }
        virgl_put(b, value);
    }
}

/* ---- state binding and drawing ---- */

static inline void virgl_bind(struct virgl_cmdbuf *b, uint32_t object,
                              uint32_t handle) {
    virgl_packet(b, VIRGL_CCMD_BIND_OBJECT, object, 1);
    virgl_put(b, handle);
}

static inline void virgl_bind_shader(struct virgl_cmdbuf *b, uint32_t handle,
                                     uint32_t stage) {
    virgl_packet(b, VIRGL_CCMD_BIND_SHADER, 0, 2);
    virgl_put(b, handle);
    virgl_put(b, stage);
}

static inline void virgl_set_framebuffer(struct virgl_cmdbuf *b,
                                         uint32_t surface_handle) {
    virgl_packet(b, VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 1 + 2);
    virgl_put(b, 1); /* nr_cbufs */
    virgl_put(b, 0); /* zsurf */
    virgl_put(b, surface_handle);
}

/* Maps clip space to the render target in plain OpenGL orientation.
 *
 * Deliberately no Y flip here: orientation belongs in one place, and the
 * compositor already emits quads in top-left screen order with flipped
 * texcoords.  Flipping here as well cancels out and silently un-flips the
 * result -- exactly the bug this comment exists to prevent. */
static inline void virgl_set_viewport(struct virgl_cmdbuf *b, int width,
                                      int height) {
    float half_w = (float)width * 0.5f;
    float half_h = (float)height * 0.5f;
    virgl_packet(b, VIRGL_CCMD_SET_VIEWPORT_STATE, 0, (6 * 1) + 1);
    virgl_put(b, 0); /* start slot */
    virgl_put(b, virgl_f32(half_w));
    virgl_put(b, virgl_f32(half_h));
    virgl_put(b, virgl_f32(1.0f));
    virgl_put(b, virgl_f32(half_w));
    virgl_put(b, virgl_f32(half_h));
    virgl_put(b, virgl_f32(0.0f));
}

static inline void virgl_clear(struct virgl_cmdbuf *b, float r, float g,
                               float blue, float a) {
    virgl_packet(b, VIRGL_CCMD_CLEAR, 0, 8);
    virgl_put(b, VIRGL_CLEAR_COLOR0);
    virgl_put(b, virgl_f32(r));
    virgl_put(b, virgl_f32(g));
    virgl_put(b, virgl_f32(blue));
    virgl_put(b, virgl_f32(a));
    virgl_put(b, 0); /* depth, low dword of binary64 */
    virgl_put(b, 0); /* depth, high dword */
    virgl_put(b, 0); /* stencil */
}

static inline void virgl_set_vertex_buffer(struct virgl_cmdbuf *b,
                                           uint32_t resource, uint32_t stride,
                                           uint32_t offset) {
    virgl_packet(b, VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3);
    virgl_put(b, stride);
    virgl_put(b, offset);
    virgl_put(b, resource);
}

static inline void virgl_set_sampler_view(struct virgl_cmdbuf *b,
                                          uint32_t view_handle) {
    virgl_packet(b, VIRGL_CCMD_SET_SAMPLER_VIEWS, 0, 1 + 2);
    virgl_put(b, VIRGL_SHADER_FRAGMENT);
    virgl_put(b, 0); /* start slot */
    virgl_put(b, view_handle);
}

static inline void virgl_bind_sampler_state(struct virgl_cmdbuf *b,
                                            uint32_t sampler_handle) {
    virgl_packet(b, VIRGL_CCMD_BIND_SAMPLER_STATES, 0, 1 + 2);
    virgl_put(b, VIRGL_SHADER_FRAGMENT);
    virgl_put(b, 0); /* start slot */
    virgl_put(b, sampler_handle);
}

/* Inline constant buffer: `count` dwords of shader constants. */
static inline void virgl_set_constants(struct virgl_cmdbuf *b, uint32_t stage,
                                       const uint32_t *values,
                                       uint32_t count) {
    virgl_packet(b, VIRGL_CCMD_SET_CONSTANT_BUFFER, 0, count + 2);
    virgl_put(b, stage);
    virgl_put(b, 0); /* index */
    for (uint32_t i = 0; i < count; i++)
        virgl_put(b, values[i]);
}

static inline void virgl_draw(struct virgl_cmdbuf *b, uint32_t start,
                              uint32_t count, uint32_t mode) {
    virgl_packet(b, VIRGL_CCMD_DRAW_VBO, 0, 12);
    virgl_put(b, start);
    virgl_put(b, count);
    virgl_put(b, mode);
    virgl_put(b, 0); /* indexed */
    virgl_put(b, 1); /* instance count */
    virgl_put(b, 0); /* index bias */
    virgl_put(b, 0); /* start instance */
    virgl_put(b, 0); /* primitive restart */
    virgl_put(b, 0); /* restart index */
    virgl_put(b, 0); /* min index */
    virgl_put(b, 0xFFFFFFFFu); /* max index */
    virgl_put(b, 0); /* count from stream output */
}

#endif /* BUZZOS_VIRGL_H */
