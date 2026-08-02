#ifndef BUZZOS_GPUCOMP_H
#define BUZZOS_GPUCOMP_H

/*
 * GPU compositor backend for the BuzzOS desktop.
 *
 * The software compositor writes every window, the taskbar and all chrome into
 * one framebuffer and hands the damaged rect to the kernel.  This backend does
 * what a modern compositor does instead: each surface lives in its own GPU
 * texture, and a frame is a short list of textured quads the host GPU blends
 * onto the scanout.  Rounded corners, per-window opacity and the acrylic
 * backdrop become fragment-shader work rather than per-pixel CPU loops.
 *
 * Design constraints that shaped this:
 *
 *  - It is strictly optional.  gpucomp_available() is false when the device
 *    does not offer virgl, and every entry point is a no-op in that case.  The
 *    software path in gui.c is untouched and remains the fallback; see
 *    docs/user-gui.md, whose live-resize rules are a property of software
 *    composition and still apply there.
 *
 *  - Uploads are the expensive part, not draws.  A texture is only re-uploaded
 *    for its damaged sub-rect (gpucomp_upload_rect), so a blinking caret costs
 *    a few hundred bytes over the ring rather than a full window.
 *
 *  - Alpha.  The rest of BuzzOS stores opaque pixels as 0x00RRGGBB -- alpha
 *    zero.  Sampled with SRC_ALPHA blending that is fully transparent, so
 *    window textures are created as B8G8R8X8_UNORM, which forces alpha to 1.0
 *    and ignores the stored byte.  Per-window opacity comes from a shader
 *    constant instead.  This is the single easiest way to get a black screen;
 *    see the ALPHA CONVENTION note in virgl.h.
 */

#include <stdint.h>
#include "libc.h"
#include "virgl.h"
#include "palette.h"

enum {
    GPUCOMP_MAX_LAYERS = 24,
    /* Handles above the ones virgl.h reserves for the shared pipeline. */
    GPUCOMP_H_FS_ROUND = 32,
    GPUCOMP_H_FS_BLUR = 33,
    GPUCOMP_H_VERTS = 34,
    GPUCOMP_H_VIEW_BASE = 40,  /* one sampler view per layer */
};

struct gpucomp_layer {
    uint32_t texture;      /* resource id, 0 if unused        */
    uint32_t *pixels;      /* mapped backing, written by us   */
    int tex_w, tex_h;      /* allocated texture size          */
    int src_w, src_h;      /* live content size within it     */
    int x, y, w, h;        /* destination rect on screen      */
    int radius;            /* corner radius, 0 = square       */
    int opacity;           /* 0..255                          */
    int visible;
};

struct gpucomp {
    int ready;
    int screen_w, screen_h;
    struct gpucomp_layer layers[GPUCOMP_MAX_LAYERS];
    struct virgl_cmdbuf cmds;
};

static struct gpucomp gpucomp_state;

/* --- Shaders ---------------------------------------------------------- */

/* Pass-through vertex shader. CONST[0] = (scale_x, scale_y, off_x, off_y)
 * maps the unit quad to a screen rect; IMM[0] supplies z=0, w=1.
 * An undeclared immediate makes the host-side parse fail silently, so IMM[0]
 * is declared explicitly. */
static const char *GPUCOMP_VS =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "DCL CONST[0..0]\n"
    "DCL TEMP[0]\n"
    "IMM[0] FLT32 { 0.0000, 1.0000, 0.0000, 0.0000}\n"
    "  0: MAD TEMP[0].xy, IN[0].xyyy, CONST[0].xyyy, CONST[0].zwww\n"
    "  1: MOV TEMP[0].zw, IMM[0].xxxy\n"
    "  2: MOV OUT[0], TEMP[0]\n"
    "  3: MOV OUT[1], IN[1]\n"
    "  4: END\n";

/* Textured quad with a rounded-rect mask and uniform opacity.
 *
 * CONST[0] = (half_w, half_h, radius, opacity) in pixels.  The mask is the
 * standard rounded-box distance: fold the fragment into one quadrant, take
 * the distance from the corner arc centre, and use the fractional coverage as
 * alpha.  Doing this on the GPU is why window corners cost nothing here while
 * the software path pays a supersampled coverage test per corner pixel. */
static const char *GPUCOMP_FS_ROUND =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[0..0]\n"
    "DCL TEMP[0..3]\n"
    "IMM[0] FLT32 { 0.5000, 1.0000, 0.0000, 2.0000}\n"
    "  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    /* p = (uv - 0.5) * 2 * half_extent  -> pixel offset from centre */
    "  1: ADD TEMP[1].xy, IN[0].xyyy, -IMM[0].xxxx\n"
    "  2: MUL TEMP[1].xy, TEMP[1].xyyy, IMM[0].wwww\n"
    "  3: MUL TEMP[1].xy, TEMP[1].xyyy, CONST[0].xyyy\n"
    /* q = |p| - (half_extent - radius) */
    "  4: ABS TEMP[1].xy, TEMP[1].xyyy\n"
    "  5: ADD TEMP[2].xy, CONST[0].xyyy, -CONST[0].zzzz\n"
    "  6: ADD TEMP[1].xy, TEMP[1].xyyy, -TEMP[2].xyyy\n"
    "  7: MAX TEMP[1].xy, TEMP[1].xyyy, IMM[0].zzzz\n"
    /* d = length(max(q,0)) - radius */
    "  8: DP2 TEMP[3].x, TEMP[1].xyyy, TEMP[1].xyyy\n"
    "  9: RSQ TEMP[3].y, TEMP[3].xxxx\n"
    " 10: RCP TEMP[3].y, TEMP[3].yyyy\n"
    /* RSQ of 0 is undefined; clamp the degenerate centre case to 0. */
    " 11: SGT TEMP[3].z, TEMP[3].xxxx, IMM[0].zzzz\n"
    " 12: MUL TEMP[3].y, TEMP[3].yyyy, TEMP[3].zzzz\n"
    " 13: ADD TEMP[3].x, TEMP[3].yyyy, -CONST[0].zzzz\n"
    /* coverage = clamp(0.5 - d, 0, 1): one pixel of linear AA at the edge */
    " 14: SUB TEMP[3].x, IMM[0].xxxx, TEMP[3].xxxx\n"
    " 15: MOV_SAT TEMP[3].x, TEMP[3].xxxx\n"
    " 16: MUL TEMP[3].x, TEMP[3].xxxx, CONST[0].wwww\n"
    " 17: MOV OUT[0], TEMP[0]\n"
    " 18: MUL OUT[0].w, TEMP[0].wwww, TEMP[3].xxxx\n"
    " 19: END\n";

/* Separable blur tap used for the acrylic backdrop.  CONST[0].xy is the
 * texel step; nine taps at 1/4 resolution match what the software path
 * approximates with three box passes. */
static const char *GPUCOMP_FS_BLUR =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[0..0]\n"
    "DCL TEMP[0..2]\n"
    "IMM[0] FLT32 { 0.2000, 1.0000, 2.0000, 3.0000}\n"
    "  0: MOV TEMP[2], IN[0]\n"
    "  1: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  2: ADD TEMP[2].xy, IN[0].xyyy, CONST[0].xyyy\n"
    "  3: TEX TEMP[1], TEMP[2], SAMP[0], 2D\n"
    "  4: ADD TEMP[0], TEMP[0], TEMP[1]\n"
    "  5: ADD TEMP[2].xy, IN[0].xyyy, -CONST[0].xyyy\n"
    "  6: TEX TEMP[1], TEMP[2], SAMP[0], 2D\n"
    "  7: ADD TEMP[0], TEMP[0], TEMP[1]\n"
    "  8: MAD TEMP[2].xy, CONST[0].xyyy, IMM[0].zzzz, IN[0].xyyy\n"
    "  9: TEX TEMP[1], TEMP[2], SAMP[0], 2D\n"
    " 10: ADD TEMP[0], TEMP[0], TEMP[1]\n"
    " 11: MOV TEMP[2].xy, -CONST[0].xyyy\n"
    " 12: MAD TEMP[2].xy, TEMP[2].xyyy, IMM[0].zzzz, IN[0].xyyy\n"
    " 13: TEX TEMP[1], TEMP[2], SAMP[0], 2D\n"
    " 14: ADD TEMP[0], TEMP[0], TEMP[1]\n"
    " 15: MUL OUT[0], TEMP[0], IMM[0].xxxx\n"
    " 16: END\n";

/* --- Setup ------------------------------------------------------------- */

static inline uint32_t gpucomp_f32(float v) { return virgl_f32(v); }

/* Unit quad: position in [0,1], texcoord v flipped so texture row 0 lands at
 * the top.  Orientation lives here and nowhere else -- flipping in both the
 * viewport and the rect math cancels out and wastes an afternoon. */
static inline void gpucomp_fill_quad(float *v) {
    static const float corners[4][4] = {
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 1.0f, 0.0f},
    };
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++)
            v[i * 4 + k] = corners[i][k];
}

static inline int gpucomp_available(void) {
    return gpucomp_state.ready;
}

/* Build the pipeline once: shaders, state objects, and the shared quad. */
static inline int gpucomp_init(int screen_w, int screen_h) {
    struct gpucomp *g = &gpucomp_state;
    struct gpu3d_caps caps;
    struct gpu3d_resource verts;

    g->ready = 0;
    if (gpu3d_info(&caps) < 0 || !caps.available)
        return -1;
    g->screen_w = screen_w;
    g->screen_h = screen_h;

    if (gpu3d_resource_create(VIRGL_TARGET_BUFFER, VIRGL_FORMAT_R32G32_FLOAT,
                              VIRGL_BIND_VERTEX_BUFFER, 4 * 4 * 4, 1,
                              &verts) < 0)
        return -1;
    gpucomp_fill_quad((float *)verts.pixels);
    if (gpu3d_upload(verts.id, 0, 0, 4 * 4 * 4, 1) < 0)
        return -1;
    g->layers[0].texture = verts.id; /* parked; see gpucomp_vertex_id */

    virgl_reset(&g->cmds);
    virgl_create_surface(&g->cmds, VIRGL_H_SURFACE, caps.scanout_resource,
                         VIRGL_FORMAT_B8G8R8A8_UNORM);
    virgl_create_blend(&g->cmds, VIRGL_H_BLEND, 1);
    virgl_create_rasterizer(&g->cmds, VIRGL_H_RASTERIZER);
    virgl_create_dsa(&g->cmds, VIRGL_H_DSA);
    virgl_create_sampler_state(&g->cmds, VIRGL_H_SAMPLER_STATE, 1);
    virgl_create_vertex_elements(&g->cmds, VIRGL_H_VERTEX_ELEMENTS);
    virgl_create_shader(&g->cmds, VIRGL_H_VS, VIRGL_SHADER_VERTEX,
                        GPUCOMP_VS);
    virgl_create_shader(&g->cmds, GPUCOMP_H_FS_ROUND, VIRGL_SHADER_FRAGMENT,
                        GPUCOMP_FS_ROUND);
    virgl_create_shader(&g->cmds, GPUCOMP_H_FS_BLUR, VIRGL_SHADER_FRAGMENT,
                        GPUCOMP_FS_BLUR);
    if (virgl_flush(&g->cmds) < 0)
        return -1;

    g->ready = 1;
    return 0;
}

static inline uint32_t gpucomp_vertex_id(void) {
    return gpucomp_state.layers[0].texture;
}

/* --- Layers ------------------------------------------------------------ */

/* Allocate or grow a layer's texture.  Textures are rounded up so a live
 * resize does not reallocate on every mouse sample. */
static inline int gpucomp_layer_ensure(int index, int w, int h) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *l;
    struct gpu3d_resource res;
    int want_w, want_h;

    if (!g->ready || index < 1 || index >= GPUCOMP_MAX_LAYERS ||
        w <= 0 || h <= 0)
        return -1;
    l = &g->layers[index];
    if (l->texture && l->tex_w >= w && l->tex_h >= h) {
        l->src_w = w;
        l->src_h = h;
        return 0;
    }
    want_w = w + w / 4;
    want_h = h + h / 4;
    if (l->texture) {
        gpu3d_resource_destroy(l->texture);
        l->texture = 0;
        l->pixels = 0;
    }
    /* B8G8R8X8 forces alpha to 1.0, so the 0x00RRGGBB convention used
     * everywhere else in the GUI does not read as fully transparent. */
    if (gpu3d_resource_create(VIRGL_TARGET_TEXTURE_2D,
                              VIRGL_FORMAT_B8G8R8X8_UNORM,
                              VIRGL_BIND_SAMPLER_VIEW,
                              (uint32_t)want_w, (uint32_t)want_h, &res) < 0)
        return -1;
    l->texture = res.id;
    l->pixels = res.pixels;
    l->tex_w = want_w;
    l->tex_h = want_h;
    l->src_w = w;
    l->src_h = h;

    virgl_reset(&g->cmds);
    virgl_create_sampler_view(&g->cmds, GPUCOMP_H_VIEW_BASE + index, res.id,
                              VIRGL_FORMAT_B8G8R8X8_UNORM);
    return virgl_flush(&g->cmds);
}

static inline void gpucomp_layer_place(int index, int x, int y, int w, int h,
                                       int radius, int opacity) {
    struct gpucomp_layer *l;
    if (index < 1 || index >= GPUCOMP_MAX_LAYERS)
        return;
    l = &gpucomp_state.layers[index];
    l->x = x; l->y = y; l->w = w; l->h = h;
    l->radius = radius;
    l->opacity = opacity;
    l->visible = 1;
}

static inline void gpucomp_layer_hide(int index) {
    if (index >= 1 && index < GPUCOMP_MAX_LAYERS)
        gpucomp_state.layers[index].visible = 0;
}

static inline uint32_t *gpucomp_layer_pixels(int index, int *stride) {
    struct gpucomp_layer *l;
    if (index < 1 || index >= GPUCOMP_MAX_LAYERS)
        return 0;
    l = &gpucomp_state.layers[index];
    if (stride)
        *stride = l->tex_w;
    return l->pixels;
}

/* Push only the damaged sub-rect of a layer to the host. */
static inline int gpucomp_upload_rect(int index, int x, int y, int w, int h) {
    struct gpucomp_layer *l;
    if (index < 1 || index >= GPUCOMP_MAX_LAYERS)
        return -1;
    l = &gpucomp_state.layers[index];
    if (!l->texture)
        return -1;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > l->tex_w) w = l->tex_w - x;
    if (y + h > l->tex_h) h = l->tex_h - y;
    if (w <= 0 || h <= 0)
        return 0;
    return gpu3d_upload(l->texture, x, y, w, h);
}

/* --- Frame ------------------------------------------------------------- */

/* Map a screen rect to the vertex shader's MAD constants.  Screen y grows
 * downward and clip space y grows upward, hence the negated scale. */
static inline void gpucomp_rect_consts(uint32_t *out, int x, int y, int w,
                                       int h) {
    struct gpucomp *g = &gpucomp_state;
    float sw = (float)g->screen_w, sh = (float)g->screen_h;
    out[0] = gpucomp_f32((float)w * 2.0f / sw);
    out[1] = gpucomp_f32(-((float)h * 2.0f / sh));
    out[2] = gpucomp_f32((float)x * 2.0f / sw - 1.0f);
    out[3] = gpucomp_f32(1.0f - (float)y * 2.0f / sh);
}

static inline void gpucomp_begin(void) {
    struct gpucomp *g = &gpucomp_state;
    if (!g->ready)
        return;
    virgl_reset(&g->cmds);
    virgl_set_framebuffer(&g->cmds, VIRGL_H_SURFACE);
    virgl_set_viewport(&g->cmds, g->screen_w, g->screen_h);
    virgl_bind(&g->cmds, VIRGL_OBJECT_BLEND, VIRGL_H_BLEND);
    virgl_bind(&g->cmds, VIRGL_OBJECT_RASTERIZER, VIRGL_H_RASTERIZER);
    virgl_bind(&g->cmds, VIRGL_OBJECT_DSA, VIRGL_H_DSA);
    virgl_bind(&g->cmds, VIRGL_OBJECT_VERTEX_ELEMENTS,
               VIRGL_H_VERTEX_ELEMENTS);
    virgl_bind_shader(&g->cmds, VIRGL_H_VS, VIRGL_SHADER_VERTEX);
    virgl_bind_shader(&g->cmds, GPUCOMP_H_FS_ROUND, VIRGL_SHADER_FRAGMENT);
    virgl_set_vertex_buffer(&g->cmds, gpucomp_vertex_id(), 4 * 4, 0);
    virgl_bind_sampler_state(&g->cmds, VIRGL_H_SAMPLER_STATE);
}

/* Queue one layer as a textured, rounded, alpha-blended quad. */
static inline void gpucomp_draw_layer(int index) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *l;
    uint32_t consts[4];

    if (!g->ready || index < 1 || index >= GPUCOMP_MAX_LAYERS)
        return;
    l = &g->layers[index];
    if (!l->visible || !l->texture || l->w <= 0 || l->h <= 0)
        return;

    virgl_set_sampler_view(&g->cmds, GPUCOMP_H_VIEW_BASE + index);
    gpucomp_rect_consts(consts, l->x, l->y, l->w, l->h);
    virgl_set_constants(&g->cmds, VIRGL_SHADER_VERTEX, consts, 4);

    /* Fragment constants are in pixels: half extent, radius, opacity. */
    consts[0] = gpucomp_f32((float)l->w * 0.5f);
    consts[1] = gpucomp_f32((float)l->h * 0.5f);
    consts[2] = gpucomp_f32((float)l->radius);
    consts[3] = gpucomp_f32((float)l->opacity / 255.0f);
    virgl_set_constants(&g->cmds, VIRGL_SHADER_FRAGMENT, consts, 4);
    virgl_draw(&g->cmds, 0, 4, VIRGL_PRIM_TRIANGLE_STRIP);
}

/* --- Acrylic ------------------------------------------------------------
 *
 * NOT IMPLEMENTED HERE, deliberately.
 *
 * The backdrop blur is the most expensive thing the software compositor does:
 * measured on a fast host, one 480x460 Start-menu acrylic costs ~1.5 ms and
 * the full-width taskbar ~0.4 ms, against 0.07 ms for a full-screen fill.  On
 * a -mno-sse i386 guest that gap is far wider, so this is the part most worth
 * moving to the GPU.
 *
 * Doing it properly needs render-to-texture: pass one blurs horizontally into
 * an offscreen target, pass two samples *that* vertically.  Both passes have
 * to bind a framebuffer backed by a scratch texture rather than the scanout,
 * which means CREATE_OBJECT(SURFACE) against a second resource and a
 * SET_FRAMEBUFFER_STATE between the passes -- neither of which the encoder
 * currently emits.  Faking it by drawing twice straight onto the scanout
 * would blur the first pass' own output in place and produce a smear, not a
 * Gaussian.
 *
 * Until that lands, ui_acrylic on the CPU remains the only implementation and
 * the shader above is unused scaffolding for it.
 */

static inline int gpucomp_end(int x, int y, int w, int h) {
    struct gpucomp *g = &gpucomp_state;
    if (!g->ready)
        return -1;
    if (virgl_flush(&g->cmds) < 0)
        return -1;
    return gpu3d_present(x, y, w, h);
}

static inline void gpucomp_shutdown(void) {
    struct gpucomp *g = &gpucomp_state;
    for (int i = 0; i < GPUCOMP_MAX_LAYERS; i++) {
        if (g->layers[i].texture)
            gpu3d_resource_destroy(g->layers[i].texture);
        g->layers[i].texture = 0;
        g->layers[i].pixels = 0;
    }
    if (g->ready)
        gpu3d_scanout(0);
    g->ready = 0;
}

#endif
