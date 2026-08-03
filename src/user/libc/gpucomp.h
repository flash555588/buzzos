#ifndef BUZZOS_GPUCOMP_H
#define BUZZOS_GPUCOMP_H

/*
 * GPU compositor backend for the BuzzOS desktop.
 *
 * The software compositor writes every window, the taskbar and all chrome into
 * one framebuffer and hands the damaged rect to the kernel.  This backend does
 * what a modern compositor does instead: each surface lives in its own GPU
 * texture, and a frame is a short list of textured quads the host GPU blends
 * onto the scanout.  Rounded corners, per-window opacity, app scaling and
 * GPU-Canvas widgets become fragment-shader work rather than CPU pixel loops.
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
    GPUCOMP_H_FS_SOLID = 35,
    GPUCOMP_H_FS_GLYPH = 36,
    GPUCOMP_H_FS_ACRYLIC = 37,
    GPUCOMP_H_VIEW_BASE = 40,  /* one sampler view per layer */
    GPUCOMP_H_FONT_VIEW = 72,
    GPUCOMP_H_CANVAS_SURFACE_BASE = 80,
    GPUCOMP_FONT_CELL = 32,
    GPUCOMP_FONT_COLS = 32,
    GPUCOMP_FONT_ROWS = 32,
    GPUCOMP_FONT_SLOTS = GPUCOMP_FONT_COLS * GPUCOMP_FONT_ROWS,
    GPUCOMP_TEXTURE_MAX_W = 1920,
    GPUCOMP_TEXTURE_MAX_H = 1200,
};

struct gpucomp_layer {
    uint32_t texture;      /* resource id, 0 if unused        */
    uint32_t *pixels;      /* mapped backing, written by us   */
    int tex_w, tex_h;      /* allocated texture size          */
    int src_w, src_h;      /* live content size within it     */
    int x, y, w, h;        /* destination rect on screen      */
    int radius;            /* corner radius, 0 = square       */
    int opacity;           /* 0..255                          */
    uint32_t format;
    uint32_t bind;
    int external;          /* texture lifetime owned elsewhere */
    int render_target;
    int visible;
};

struct gpucomp {
    int ready;
    int failed;
    int screen_w, screen_h;
    int target_w, target_h;
    struct gpucomp_layer layers[GPUCOMP_MAX_LAYERS];
    uint32_t font_texture;
    uint32_t *font_pixels;
    int font_tex_w, font_tex_h;
    int font_slot_count;
    uint32_t glyph_codepoint[GPUCOMP_FONT_SLOTS];
    uint8_t glyph_width[GPUCOMP_FONT_SLOTS];
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
    "DCL CONST[0..1]\n"
    "DCL TEMP[0..4]\n"
    "IMM[0] FLT32 { 0.5000, 1.0000, 0.0000, 2.0000}\n"
    /* CONST[1] maps the unit quad into the live sub-rectangle of a
     * grow-only texture: (u_scale, v_scale, u_offset, v_offset). */
    "  0: MAD TEMP[4].xy, IN[0].xyyy, CONST[1].xyyy, CONST[1].zwww\n"
    "  1: TEX TEMP[0], TEMP[4], SAMP[0], 2D\n"
    /* p = (uv - 0.5) * 2 * half_extent  -> pixel offset from centre */
    "  2: ADD TEMP[1].xy, IN[0].xyyy, -IMM[0].xxxx\n"
    "  3: MUL TEMP[1].xy, TEMP[1].xyyy, IMM[0].wwww\n"
    "  4: MUL TEMP[1].xy, TEMP[1].xyyy, CONST[0].xyyy\n"
    /* q = |p| - (half_extent - radius) */
    "  5: ABS TEMP[1].xy, TEMP[1].xyyy\n"
    "  6: ADD TEMP[2].xy, CONST[0].xyyy, -CONST[0].zzzz\n"
    "  7: ADD TEMP[1].xy, TEMP[1].xyyy, -TEMP[2].xyyy\n"
    /* Preserve the negative, interior part of the signed distance.  Omitting
     * it makes a square (radius 0) incorrectly render at half opacity. */
    "  8: MAX TEMP[2].z, TEMP[1].xxxx, TEMP[1].yyyy\n"
    "  9: MIN TEMP[2].z, TEMP[2].zzzz, IMM[0].zzzz\n"
    " 10: MAX TEMP[1].xy, TEMP[1].xyyy, IMM[0].zzzz\n"
    /* d = length(max(q,0)) + min(max(q.x,q.y),0) - radius */
    " 11: DP2 TEMP[3].x, TEMP[1].xyyy, TEMP[1].xyyy\n"
    " 12: RSQ TEMP[3].y, TEMP[3].xxxx\n"
    " 13: RCP TEMP[3].y, TEMP[3].yyyy\n"
    /* RSQ of 0 is undefined; clamp the degenerate centre case to 0. */
    " 14: SGT TEMP[3].z, TEMP[3].xxxx, IMM[0].zzzz\n"
    " 15: MUL TEMP[3].y, TEMP[3].yyyy, TEMP[3].zzzz\n"
    " 16: ADD TEMP[3].y, TEMP[3].yyyy, TEMP[2].zzzz\n"
    " 17: ADD TEMP[3].x, TEMP[3].yyyy, -CONST[0].zzzz\n"
    /* coverage = clamp(0.5 - d, 0, 1): one pixel of linear AA at the edge */
    " 18: SUB TEMP[3].x, IMM[0].xxxx, TEMP[3].xxxx\n"
    " 19: MOV_SAT TEMP[3].x, TEMP[3].xxxx\n"
    " 20: MUL TEMP[3].x, TEMP[3].xxxx, CONST[0].wwww\n"
    " 21: MOV OUT[0], TEMP[0]\n"
    " 22: MUL OUT[0].w, TEMP[0].wwww, TEMP[3].xxxx\n"
    " 23: END\n";

/* Pure colour rounded rectangle.  It shares the same analytic edge mask as
 * the texture shader, but needs no sampler. */
static const char *GPUCOMP_FS_SOLID =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL CONST[0..1]\n"
    "DCL TEMP[0..2]\n"
    "IMM[0] FLT32 { 0.5000, 1.0000, 0.0000, 2.0000}\n"
    "  0: ADD TEMP[0].xy, IN[0].xyyy, -IMM[0].xxxx\n"
    "  1: MUL TEMP[0].xy, TEMP[0].xyyy, IMM[0].wwww\n"
    "  2: MUL TEMP[0].xy, TEMP[0].xyyy, CONST[0].xyyy\n"
    "  3: ABS TEMP[0].xy, TEMP[0].xyyy\n"
    "  4: ADD TEMP[1].xy, CONST[0].xyyy, -CONST[0].zzzz\n"
    "  5: ADD TEMP[0].xy, TEMP[0].xyyy, -TEMP[1].xyyy\n"
    "  6: MAX TEMP[1].z, TEMP[0].xxxx, TEMP[0].yyyy\n"
    "  7: MIN TEMP[1].z, TEMP[1].zzzz, IMM[0].zzzz\n"
    "  8: MAX TEMP[0].xy, TEMP[0].xyyy, IMM[0].zzzz\n"
    "  9: DP2 TEMP[2].x, TEMP[0].xyyy, TEMP[0].xyyy\n"
    " 10: RSQ TEMP[2].y, TEMP[2].xxxx\n"
    " 11: RCP TEMP[2].y, TEMP[2].yyyy\n"
    " 12: SGT TEMP[2].z, TEMP[2].xxxx, IMM[0].zzzz\n"
    " 13: MUL TEMP[2].y, TEMP[2].yyyy, TEMP[2].zzzz\n"
    " 14: ADD TEMP[2].y, TEMP[2].yyyy, TEMP[1].zzzz\n"
    " 15: ADD TEMP[2].x, TEMP[2].yyyy, -CONST[0].zzzz\n"
    " 16: SUB TEMP[2].x, IMM[0].xxxx, TEMP[2].xxxx\n"
    " 17: MOV_SAT TEMP[2].x, TEMP[2].xxxx\n"
    " 18: MOV OUT[0], CONST[1]\n"
    " 19: MUL OUT[0].w, CONST[1].wwww, TEMP[2].xxxx\n"
    " 20: END\n";

/* Alpha-only glyph atlas tinted by CONST[0].  CONST[1] maps the unit quad
 * into one atlas cell. */
static const char *GPUCOMP_FS_GLYPH =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[0..1]\n"
    "DCL TEMP[0..1]\n"
    "  0: MAD TEMP[1].xy, IN[0].xyyy, CONST[1].xyyy, CONST[1].zwww\n"
    "  1: TEX TEMP[0], TEMP[1], SAMP[0], 2D\n"
    "  2: MOV OUT[0], CONST[0]\n"
    "  3: MUL OUT[0].w, CONST[0].wwww, TEMP[0].wwww\n"
    "  4: END\n";

/* Separable blur tap used for the acrylic backdrop.  CONST[0].xy is the
 * texel step; five taps in each direction run at 1/4 resolution. */
static const char *GPUCOMP_FS_BLUR =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[0..1]\n"
    "DCL TEMP[0..2]\n"
    "IMM[0] FLT32 { 0.2000, 1.0000, 2.0000, 3.0000}\n"
    "  0: MAD TEMP[2].xy, IN[0].xyyy, CONST[1].xyyy, CONST[1].zwww\n"
    "  1: TEX TEMP[0], TEMP[2], SAMP[0], 2D\n"
    "  2: MAD TEMP[2].xy, IN[0].xyyy, CONST[1].xyyy, CONST[1].zwww\n"
    "  3: ADD TEMP[2].xy, TEMP[2].xyyy, CONST[0].xyyy\n"
    "  4: TEX TEMP[1], TEMP[2], SAMP[0], 2D\n"
    "  5: ADD TEMP[0], TEMP[0], TEMP[1]\n"
    "  6: MAD TEMP[2].xy, IN[0].xyyy, CONST[1].xyyy, CONST[1].zwww\n"
    "  7: ADD TEMP[2].xy, TEMP[2].xyyy, -CONST[0].xyyy\n"
    "  8: TEX TEMP[1], TEMP[2], SAMP[0], 2D\n"
    "  9: ADD TEMP[0], TEMP[0], TEMP[1]\n"
    " 10: MAD TEMP[2].xy, IN[0].xyyy, CONST[1].xyyy, CONST[1].zwww\n"
    " 11: MAD TEMP[2].xy, CONST[0].xyyy, IMM[0].zzzz, TEMP[2].xyyy\n"
    " 12: TEX TEMP[1], TEMP[2], SAMP[0], 2D\n"
    " 13: ADD TEMP[0], TEMP[0], TEMP[1]\n"
    " 14: MAD TEMP[2].xy, IN[0].xyyy, CONST[1].xyyy, CONST[1].zwww\n"
    " 15: MOV TEMP[1].xy, -CONST[0].xyyy\n"
    " 16: MAD TEMP[2].xy, TEMP[1].xyyy, IMM[0].zzzz, TEMP[2].xyyy\n"
    " 17: TEX TEMP[1], TEMP[2], SAMP[0], 2D\n"
    " 18: ADD TEMP[0], TEMP[0], TEMP[1]\n"
    " 19: MUL OUT[0], TEMP[0], IMM[0].xxxx\n"
    " 20: END\n";

/* Sample the cached full-screen blur, tint it, and apply an analytic rounded
 * mask.  CONST[2] maps the acrylic quad to its screen-relative sub-rectangle
 * in the quarter-resolution blur texture. */
static const char *GPUCOMP_FS_ACRYLIC =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[0..2]\n"
    "DCL TEMP[0..4]\n"
    "IMM[0] FLT32 { 0.5000, 1.0000, 0.0000, 2.0000}\n"
    "  0: MAD TEMP[4].xy, IN[0].xyyy, CONST[2].xyyy, CONST[2].zwww\n"
    "  1: TEX TEMP[0], TEMP[4], SAMP[0], 2D\n"
    "  2: SUB TEMP[4].z, IMM[0].yyyy, CONST[0].wwww\n"
    "  3: MUL TEMP[0].xyz, TEMP[0], TEMP[4].zzzz\n"
    "  4: MAD TEMP[0].xyz, CONST[1], CONST[0].wwww, TEMP[0]\n"
    "  5: ADD TEMP[1].xy, IN[0].xyyy, -IMM[0].xxxx\n"
    "  6: MUL TEMP[1].xy, TEMP[1].xyyy, IMM[0].wwww\n"
    "  7: MUL TEMP[1].xy, TEMP[1].xyyy, CONST[0].xyyy\n"
    "  8: ABS TEMP[1].xy, TEMP[1].xyyy\n"
    "  9: ADD TEMP[2].xy, CONST[0].xyyy, -CONST[0].zzzz\n"
    " 10: ADD TEMP[1].xy, TEMP[1].xyyy, -TEMP[2].xyyy\n"
    " 11: MAX TEMP[2].z, TEMP[1].xxxx, TEMP[1].yyyy\n"
    " 12: MIN TEMP[2].z, TEMP[2].zzzz, IMM[0].zzzz\n"
    " 13: MAX TEMP[1].xy, TEMP[1].xyyy, IMM[0].zzzz\n"
    " 14: DP2 TEMP[3].x, TEMP[1].xyyy, TEMP[1].xyyy\n"
    " 15: RSQ TEMP[3].y, TEMP[3].xxxx\n"
    " 16: RCP TEMP[3].y, TEMP[3].yyyy\n"
    " 17: SGT TEMP[3].z, TEMP[3].xxxx, IMM[0].zzzz\n"
    " 18: MUL TEMP[3].y, TEMP[3].yyyy, TEMP[3].zzzz\n"
    " 19: ADD TEMP[3].y, TEMP[3].yyyy, TEMP[2].zzzz\n"
    " 20: ADD TEMP[3].x, TEMP[3].yyyy, -CONST[0].zzzz\n"
    " 21: SUB TEMP[3].x, IMM[0].xxxx, TEMP[3].xxxx\n"
    " 22: MOV_SAT TEMP[3].x, TEMP[3].xxxx\n"
    " 23: MOV OUT[0], TEMP[0]\n"
    " 24: MOV OUT[0].w, TEMP[3].xxxx\n"
    " 25: END\n";

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

    memset(g, 0, sizeof(*g));
    if (gpu3d_info(&caps) < 0 || !caps.available ||
        caps.width != (uint32_t)screen_w ||
        caps.height != (uint32_t)screen_h)
        return -1;
    g->screen_w = screen_w;
    g->screen_h = screen_h;

    if (gpu3d_resource_create(VIRGL_TARGET_BUFFER, VIRGL_FORMAT_R32G32_FLOAT,
                              VIRGL_BIND_VERTEX_BUFFER, 4 * 4 * 4, 1,
                              &verts) < 0)
        return -1;
    g->layers[0].texture = verts.id; /* parked; see gpucomp_vertex_id */
    gpucomp_fill_quad((float *)verts.pixels);
    if (gpu3d_upload(verts.id, 0, 0, 4 * 4 * 4, 1) < 0) {
        (void)gpu3d_resource_destroy(verts.id);
        g->layers[0].texture = 0;
        return -1;
    }

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
    virgl_create_shader(&g->cmds, GPUCOMP_H_FS_ACRYLIC,
                        VIRGL_SHADER_FRAGMENT, GPUCOMP_FS_ACRYLIC);
    virgl_create_shader(&g->cmds, GPUCOMP_H_FS_SOLID,
                        VIRGL_SHADER_FRAGMENT, GPUCOMP_FS_SOLID);
    virgl_create_shader(&g->cmds, GPUCOMP_H_FS_GLYPH,
                        VIRGL_SHADER_FRAGMENT, GPUCOMP_FS_GLYPH);
    if (virgl_flush(&g->cmds) < 0) {
        (void)gpu3d_resource_destroy(verts.id);
        g->layers[0].texture = 0;
        return -1;
    }

    g->ready = 1;
    g->target_w = screen_w;
    g->target_h = screen_h;
    return 0;
}

static inline uint32_t gpucomp_vertex_id(void) {
    return gpucomp_state.layers[0].texture;
}

static inline int gpucomp_layer_release(int index, int destroy_external) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *l;
    if (index < 1 || index >= GPUCOMP_MAX_LAYERS)
        return -1;
    l = &g->layers[index];
    if (!l->texture)
        return 0;
    if (g->ready) {
        virgl_reset(&g->cmds);
        if (l->render_target)
            virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SURFACE,
                                 GPUCOMP_H_CANVAS_SURFACE_BASE + index);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SAMPLER_VIEW,
                             GPUCOMP_H_VIEW_BASE + index);
        (void)virgl_flush(&g->cmds);
    }
    if (!l->external || destroy_external)
        (void)gpu3d_resource_destroy(l->texture);
    memset(l, 0, sizeof(*l));
    return 0;
}

/* --- Layers ------------------------------------------------------------ */

/* Allocate or grow a layer's texture.  Textures are rounded up so a live
 * resize does not reallocate on every mouse sample. */
static inline int gpucomp_layer_ensure_bind(int index, int w, int h,
                                            uint32_t format, uint32_t bind,
                                            int render_target, int exact) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *l;
    struct gpu3d_resource res;
    int want_w, want_h;

    if (!g->ready || index < 1 || index >= GPUCOMP_MAX_LAYERS ||
        w <= 0 || h <= 0)
        return -1;
    l = &g->layers[index];
    if (l->texture && !l->external && l->format == format &&
        l->bind == bind && l->render_target == render_target &&
        ((exact && l->tex_w == w && l->tex_h == h) ||
         (!exact && l->tex_w >= w && l->tex_h >= h))) {
        l->src_w = w;
        l->src_h = h;
        return 0;
    }
    want_w = exact ? w : w + w / 4;
    want_h = exact ? h : h + h / 4;
    if (want_w > GPUCOMP_TEXTURE_MAX_W) want_w = GPUCOMP_TEXTURE_MAX_W;
    if (want_h > GPUCOMP_TEXTURE_MAX_H) want_h = GPUCOMP_TEXTURE_MAX_H;
    if (l->texture)
        (void)gpucomp_layer_release(index, 0);
    if (gpu3d_resource_create(VIRGL_TARGET_TEXTURE_2D,
                              format,
                              bind,
                              (uint32_t)want_w, (uint32_t)want_h, &res) < 0)
        return -1;
    l->texture = res.id;
    l->pixels = res.pixels;
    l->tex_w = want_w;
    l->tex_h = want_h;
    l->src_w = w;
    l->src_h = h;
    l->format = format;
    l->bind = bind;
    l->external = 0;
    l->render_target = render_target;

    virgl_reset(&g->cmds);
    virgl_create_sampler_view(&g->cmds, GPUCOMP_H_VIEW_BASE + index, res.id,
                              format);
    if (render_target)
        virgl_create_surface(&g->cmds,
                             GPUCOMP_H_CANVAS_SURFACE_BASE + index,
                             res.id, format);
    return virgl_flush(&g->cmds);
}

static inline int gpucomp_layer_ensure_format(int index, int w, int h,
                                               uint32_t format) {
    return gpucomp_layer_ensure_bind(index, w, h, format,
                                     VIRGL_BIND_SAMPLER_VIEW, 0, 0);
}

static inline int gpucomp_layer_ensure(int index, int w, int h) {
    /* B8G8R8X8 forces alpha to 1.0, so the 0x00RRGGBB convention used
     * everywhere else in the GUI does not read as fully transparent. */
    return gpucomp_layer_ensure_format(index, w, h,
                                       VIRGL_FORMAT_B8G8R8X8_UNORM);
}

/* Attach a resource whose backing/lifetime is managed by the GUI session.
 * No pixels are copied and gpucomp_shutdown will not destroy the resource. */
static inline int gpucomp_layer_import(int index, uint32_t resource,
                                       int w, int h, uint32_t format) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *l;
    if (!g->ready || index < 1 || index >= GPUCOMP_MAX_LAYERS ||
        !resource || w <= 0 || h <= 0)
        return -1;
    l = &g->layers[index];
    if (l->texture == resource && l->external && l->format == format &&
        l->tex_w == w && l->tex_h == h) {
        l->src_w = w;
        l->src_h = h;
        return 0;
    }
    if (l->texture)
        (void)gpucomp_layer_release(index, 0);
    l->texture = resource;
    l->tex_w = w;
    l->tex_h = h;
    l->src_w = w;
    l->src_h = h;
    l->format = format;
    l->bind = VIRGL_BIND_SAMPLER_VIEW;
    l->external = 1;
    l->render_target = 0;
    virgl_reset(&g->cmds);
    virgl_create_sampler_view(&g->cmds, GPUCOMP_H_VIEW_BASE + index,
                              resource, format);
    if (virgl_flush(&g->cmds) < 0) {
        memset(l, 0, sizeof(*l));
        return -1;
    }
    return 0;
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

static inline uint32_t gpucomp_layer_resource(int index) {
    if (index < 1 || index >= GPUCOMP_MAX_LAYERS)
        return 0;
    return gpucomp_state.layers[index].texture;
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

static inline void gpucomp_rect_consts_target(uint32_t *out, int x, int y,
                                              int w, int h, int target_w,
                                              int target_h) {
    float tw = (float)target_w, th = (float)target_h;
    out[0] = gpucomp_f32((float)w * 2.0f / tw);
    out[1] = gpucomp_f32(-((float)h * 2.0f / th));
    out[2] = gpucomp_f32((float)x * 2.0f / tw - 1.0f);
    out[3] = gpucomp_f32(1.0f - (float)y * 2.0f / th);
}

static inline int gpucomp_canvas_ensure(int index, int w, int h) {
    return gpucomp_layer_ensure_bind(
        index, w, h, VIRGL_FORMAT_B8G8R8X8_UNORM,
        VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW, 1, 0);
}

static inline int gpucomp_target_ensure(int index, int w, int h) {
    return gpucomp_layer_ensure_bind(
        index, w, h, VIRGL_FORMAT_B8G8R8A8_UNORM,
        VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW, 1, 1);
}

static inline int gpucomp_font_ensure(void) {
    enum { TEX_W = GPUCOMP_FONT_CELL * GPUCOMP_FONT_COLS,
           TEX_H = GPUCOMP_FONT_CELL * GPUCOMP_FONT_ROWS };
    struct gpucomp *g = &gpucomp_state;
    struct gpu3d_resource atlas;
    uint8_t bits[FONT_GLYPH_BYTES];
    if (g->font_texture)
        return 0;
    if (!g->ready || gpu3d_resource_create(
            VIRGL_TARGET_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM,
            VIRGL_BIND_SAMPLER_VIEW, TEX_W, TEX_H, &atlas) < 0)
        return -1;
    memset(atlas.pixels, 0, (size_t)atlas.bytes);
    for (int i = 0; i < 96; i++) {
        memset(bits, 0, sizeof(bits));
        int glyph_w = font_glyph((uint32_t)(i + 32), bits, sizeof(bits));
        if (glyph_w <= 0 || glyph_w > FONT_GLYPH_MAX_WIDTH)
            glyph_w = font_glyph('?', bits, sizeof(bits));
        if (glyph_w <= 0)
            glyph_w = 8;
        g->glyph_width[i] = (uint8_t)glyph_w;
        g->glyph_codepoint[i] = (uint32_t)(i + 32);
        int ox = (i % GPUCOMP_FONT_COLS) * GPUCOMP_FONT_CELL;
        int oy = (i / GPUCOMP_FONT_COLS) * GPUCOMP_FONT_CELL;
        for (int y = 0; y < FONT_GLYPH_HEIGHT; y++)
            for (int x = 0; x < glyph_w; x++)
                if (bits[y * FONT_GLYPH_STRIDE + x / 8] &
                    (uint8_t)(0x80u >> (x & 7)))
                    atlas.pixels[(oy + y) * TEX_W + ox + x] = 0xFFFFFFFFu;
    }
    if (gpu3d_upload(atlas.id, 0, 0, TEX_W, TEX_H) < 0) {
        (void)gpu3d_resource_destroy(atlas.id);
        return -1;
    }
    virgl_reset(&g->cmds);
    virgl_create_sampler_view(&g->cmds, GPUCOMP_H_FONT_VIEW, atlas.id,
                              VIRGL_FORMAT_B8G8R8A8_UNORM);
    if (virgl_flush(&g->cmds) < 0) {
        (void)gpu3d_resource_destroy(atlas.id);
        return -1;
    }
    g->font_texture = atlas.id;
    g->font_pixels = atlas.pixels;
    g->font_tex_w = TEX_W;
    g->font_tex_h = TEX_H;
    g->font_slot_count = 96;
    return 0;
}

static inline uint32_t gpucomp_utf8_next(const char *text, int length,
                                          int *offset) {
    const uint8_t *s = (const uint8_t *)text;
    int i = *offset;
    uint32_t cp;
    int extra;
    if (i >= length)
        return 0;
    uint8_t first = s[i++];
    if (first < 0x80u) {
        *offset = i;
        return first;
    }
    if (first >= 0xC2u && first <= 0xDFu) {
        cp = first & 0x1Fu;
        extra = 1;
    } else if (first >= 0xE0u && first <= 0xEFu) {
        cp = first & 0x0Fu;
        extra = 2;
    } else if (first >= 0xF0u && first <= 0xF4u) {
        cp = first & 0x07u;
        extra = 3;
    } else {
        *offset = i;
        return 0xFFFDu;
    }
    if (i + extra > length) {
        *offset = i;
        return 0xFFFDu;
    }
    for (int n = 0; n < extra; n++) {
        uint8_t next = s[i + n];
        if ((next & 0xC0u) != 0x80u) {
            *offset = i;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (next & 0x3Fu);
    }
    if ((extra == 2 && cp < 0x800u) ||
        (extra == 3 && cp < 0x10000u) ||
        (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
        *offset = i;
        return 0xFFFDu;
    }
    *offset = i + extra;
    return cp;
}

/* Cache each codepoint once in a host-sampled atlas.  Unicode rasterization
 * is therefore a one-time glyph upload, never a per-frame CPU pixel walk. */
static inline int gpucomp_font_slot(uint32_t codepoint) {
    struct gpucomp *g = &gpucomp_state;
    uint8_t bits[FONT_GLYPH_BYTES];
    if (codepoint < 32u || codepoint == 127u)
        codepoint = ' ';
    if (codepoint >= 32u && codepoint < 128u)
        return (int)codepoint - 32;
    for (int slot = 96; slot < g->font_slot_count; slot++)
        if (g->glyph_codepoint[slot] == codepoint)
            return slot;
    if (g->font_slot_count >= GPUCOMP_FONT_SLOTS)
        return '?' - 32;

    memset(bits, 0, sizeof(bits));
    int glyph_w = font_glyph(codepoint, bits, sizeof(bits));
    if (glyph_w <= 0 || glyph_w > FONT_GLYPH_MAX_WIDTH)
        return '?' - 32;
    int slot = g->font_slot_count++;
    int ox = (slot % GPUCOMP_FONT_COLS) * GPUCOMP_FONT_CELL;
    int oy = (slot / GPUCOMP_FONT_COLS) * GPUCOMP_FONT_CELL;
    for (int y = 0; y < GPUCOMP_FONT_CELL; y++)
        for (int x = 0; x < GPUCOMP_FONT_CELL; x++)
            g->font_pixels[(oy + y) * g->font_tex_w + ox + x] = 0;
    for (int y = 0; y < FONT_GLYPH_HEIGHT; y++)
        for (int x = 0; x < glyph_w; x++)
            if (bits[y * FONT_GLYPH_STRIDE + x / 8] &
                (uint8_t)(0x80u >> (x & 7)))
                g->font_pixels[(oy + y) * g->font_tex_w + ox + x] =
                    0xFFFFFFFFu;
    if (gpu3d_upload(g->font_texture, ox, oy, GPUCOMP_FONT_CELL,
                     GPUCOMP_FONT_CELL) < 0) {
        g->font_slot_count--;
        return -1;
    }
    g->glyph_codepoint[slot] = codepoint;
    g->glyph_width[slot] = (uint8_t)glyph_w;
    return slot;
}

static inline int gpucomp_font_prepare_text(const char *text, int length) {
    int offset = 0;
    while (offset < length) {
        uint32_t cp = gpucomp_utf8_next(text, length, &offset);
        if (gpucomp_font_slot(cp) < 0)
            return -1;
    }
    return 0;
}

static inline int gpucomp_canvas_room(struct gpucomp *g, uint32_t dwords) {
    if (g->cmds.overflow)
        return -1;
    if (g->cmds.len + dwords < VIRGL_CMD_CAPACITY)
        return 0;
    return virgl_flush(&g->cmds);
}

static inline void gpucomp_canvas_scissor(struct gpucomp_layer *l,
                                          int x, int y, int w, int h) {
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + w;
    int y2 = y + h;
    if (x2 > l->src_w) x2 = l->src_w;
    if (y2 > l->src_h) y2 = l->src_h;
    if (x2 < x1) x2 = x1;
    if (y2 < y1) y2 = y1;
    virgl_set_scissor(&gpucomp_state.cmds, x1, l->tex_h - y2,
                      x2, l->tex_h - y1);
}

static inline int gpucomp_canvas_begin(int index) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *l;
    if (!g->ready || index < 1 || index >= GPUCOMP_MAX_LAYERS ||
        gpucomp_font_ensure() < 0)
        return -1;
    l = &g->layers[index];
    if (!l->texture || !l->render_target)
        return -1;
    virgl_reset(&g->cmds);
    virgl_set_framebuffer(&g->cmds,
                          GPUCOMP_H_CANVAS_SURFACE_BASE + index);
    virgl_set_viewport(&g->cmds, l->tex_w, l->tex_h);
    gpucomp_canvas_scissor(l, 0, 0, l->src_w, l->src_h);
    virgl_bind(&g->cmds, VIRGL_OBJECT_BLEND, VIRGL_H_BLEND);
    virgl_bind(&g->cmds, VIRGL_OBJECT_RASTERIZER, VIRGL_H_RASTERIZER);
    virgl_bind(&g->cmds, VIRGL_OBJECT_DSA, VIRGL_H_DSA);
    virgl_bind(&g->cmds, VIRGL_OBJECT_VERTEX_ELEMENTS,
               VIRGL_H_VERTEX_ELEMENTS);
    virgl_bind_shader(&g->cmds, VIRGL_H_VS, VIRGL_SHADER_VERTEX);
    virgl_set_vertex_buffer(&g->cmds, gpucomp_vertex_id(), 4 * 4, 0);
    virgl_bind_sampler_state(&g->cmds, VIRGL_H_SAMPLER_STATE);
    virgl_clear(&g->cmds, 0.0f, 0.0f, 0.0f, 1.0f);
    return 0;
}

static inline void gpucomp_color_constants(uint32_t *out, uint32_t color) {
    out[0] = gpucomp_f32((float)((color >> 16) & 255u) / 255.0f);
    out[1] = gpucomp_f32((float)((color >> 8) & 255u) / 255.0f);
    out[2] = gpucomp_f32((float)(color & 255u) / 255.0f);
    out[3] = gpucomp_f32(1.0f);
}

static inline int gpucomp_canvas_rect(int index, int x, int y, int w, int h,
                                      int radius, uint32_t color) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *l;
    uint32_t vs[4], fs[8];
    if (!g->ready || index < 1 || index >= GPUCOMP_MAX_LAYERS ||
        w <= 0 || h <= 0)
        return -1;
    l = &g->layers[index];
    if (!l->render_target || gpucomp_canvas_room(g, 40) < 0)
        return -1;
    if (radius < 0) radius = 0;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    gpucomp_canvas_scissor(l, 0, 0, l->src_w, l->src_h);
    virgl_bind_shader(&g->cmds, GPUCOMP_H_FS_SOLID,
                      VIRGL_SHADER_FRAGMENT);
    gpucomp_rect_consts_target(vs, x, y, w, h, l->tex_w, l->tex_h);
    virgl_set_constants(&g->cmds, VIRGL_SHADER_VERTEX, vs, 4);
    fs[0] = gpucomp_f32((float)w * 0.5f);
    fs[1] = gpucomp_f32((float)h * 0.5f);
    fs[2] = gpucomp_f32((float)radius);
    fs[3] = gpucomp_f32(1.0f);
    gpucomp_color_constants(&fs[4], color);
    virgl_set_constants(&g->cmds, VIRGL_SHADER_FRAGMENT, fs, 8);
    virgl_draw(&g->cmds, 0, 4, VIRGL_PRIM_TRIANGLE_STRIP);
    return 0;
}

static inline int gpucomp_canvas_text_width(const char *text, int length,
                                             int size) {
    struct gpucomp *g = &gpucomp_state;
    int width = 0;
    int offset = 0;
    if (size < 1) size = 1;
    while (offset < length) {
        uint32_t cp = gpucomp_utf8_next(text, length, &offset);
        int slot = gpucomp_font_slot(cp);
        if (slot < 0)
            return -1;
        int gw = g->glyph_width[slot];
        width += (gw * size + FONT_GLYPH_HEIGHT - 1) /
                 FONT_GLYPH_HEIGHT + 1;
    }
    return width > 0 ? width - 1 : 0;
}

static inline int gpucomp_canvas_text(int index, int x, int y, int w, int h,
                                      const char *text, int length, int size,
                                      uint32_t color, int align, int bold) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *l;
    uint32_t vs[4], fs[8];
    int cursor, top, total, offset = 0;
    if (!g->ready || index < 1 || index >= GPUCOMP_MAX_LAYERS ||
        !text || length <= 0 || w <= 0 || h <= 0 ||
        gpucomp_font_ensure() < 0)
        return -1;
    l = &g->layers[index];
    if (!l->render_target)
        return -1;
    if (size < 6) size = 6;
    if (size > 64) size = 64;
    if (gpucomp_font_prepare_text(text, length) < 0)
        return -1;
    total = gpucomp_canvas_text_width(text, length, size);
    if (total < 0)
        return -1;
    cursor = align == 2 ? x + w - total :
             align == 1 ? x + (w - total) / 2 : x;
    top = y + (h - size) / 2;
    gpucomp_canvas_scissor(l, x, y, w, h);
    virgl_bind_shader(&g->cmds, GPUCOMP_H_FS_GLYPH,
                      VIRGL_SHADER_FRAGMENT);
    virgl_set_sampler_view(&g->cmds, GPUCOMP_H_FONT_VIEW);
    while (offset < length && cursor < x + w) {
        uint32_t cp = gpucomp_utf8_next(text, length, &offset);
        int glyph = gpucomp_font_slot(cp);
        if (glyph < 0)
            return -1;
        int gw = g->glyph_width[glyph];
        int dw = (gw * size + FONT_GLYPH_HEIGHT - 1) /
                 FONT_GLYPH_HEIGHT;
        int advance = dw + 1;
        if (cp != ' ' && cursor + dw > x && cursor < x + w) {
            for (int pass = 0; pass < (bold ? 2 : 1); pass++) {
                if (gpucomp_canvas_room(g, 44) < 0)
                    return -1;
                gpucomp_rect_consts_target(vs, cursor + pass, top, dw, size,
                                            l->tex_w, l->tex_h);
                virgl_set_constants(&g->cmds, VIRGL_SHADER_VERTEX, vs, 4);
                gpucomp_color_constants(fs, color);
                fs[4] = gpucomp_f32((float)gw / (float)g->font_tex_w);
                fs[5] = gpucomp_f32((float)FONT_GLYPH_HEIGHT /
                                    (float)g->font_tex_h);
                fs[6] = gpucomp_f32((float)((glyph % GPUCOMP_FONT_COLS) *
                                            GPUCOMP_FONT_CELL) /
                                    (float)g->font_tex_w);
                fs[7] = gpucomp_f32(1.0f -
                    (float)((glyph / GPUCOMP_FONT_COLS) * GPUCOMP_FONT_CELL +
                            FONT_GLYPH_HEIGHT) /
                    (float)g->font_tex_h);
                virgl_set_constants(&g->cmds, VIRGL_SHADER_FRAGMENT, fs, 8);
                virgl_draw(&g->cmds, 0, 4, VIRGL_PRIM_TRIANGLE_STRIP);
            }
        }
        cursor += advance;
    }
    return 0;
}

static inline int gpucomp_canvas_end(void) {
    struct gpucomp *g = &gpucomp_state;
    if (!g->ready || g->cmds.overflow)
        return -1;
    return g->cmds.len ? virgl_flush(&g->cmds) : 0;
}

static inline void gpucomp_begin(void) {
    struct gpucomp *g = &gpucomp_state;
    if (!g->ready)
        return;
    g->failed = 0;
    g->target_w = g->screen_w;
    g->target_h = g->screen_h;
    virgl_reset(&g->cmds);
    virgl_set_framebuffer(&g->cmds, VIRGL_H_SURFACE);
    virgl_set_viewport(&g->cmds, g->screen_w, g->screen_h);
    virgl_set_scissor(&g->cmds, 0, 0, g->screen_w, g->screen_h);
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
    uint32_t fs_consts[8];

    if (!g->ready || index < 1 || index >= GPUCOMP_MAX_LAYERS)
        return;
    l = &g->layers[index];
    if (!l->visible || !l->texture || l->w <= 0 || l->h <= 0)
        return;
    if (g->cmds.len + 40u >= VIRGL_CMD_CAPACITY &&
        virgl_flush(&g->cmds) < 0) {
        g->failed = 1;
        return;
    }

    virgl_set_sampler_view(&g->cmds, GPUCOMP_H_VIEW_BASE + index);
    virgl_bind_shader(&g->cmds, GPUCOMP_H_FS_ROUND,
                      VIRGL_SHADER_FRAGMENT);
    gpucomp_rect_consts_target(consts, l->x, l->y, l->w, l->h,
                               g->target_w, g->target_h);
    virgl_set_constants(&g->cmds, VIRGL_SHADER_VERTEX, consts, 4);

    /* Fragment constants are in pixels: half extent, radius, opacity. */
    fs_consts[0] = gpucomp_f32((float)l->w * 0.5f);
    fs_consts[1] = gpucomp_f32((float)l->h * 0.5f);
    fs_consts[2] = gpucomp_f32((float)l->radius);
    fs_consts[3] = gpucomp_f32((float)l->opacity / 255.0f);
    fs_consts[4] = gpucomp_f32((float)l->src_w / (float)l->tex_w);
    fs_consts[5] = gpucomp_f32((float)l->src_h / (float)l->tex_h);
    fs_consts[6] = gpucomp_f32(0.0f);
    /* Texture row zero is the top row while virgl's v=1 is the top edge. */
    fs_consts[7] = gpucomp_f32(1.0f -
                               (float)l->src_h / (float)l->tex_h);
    virgl_set_constants(&g->cmds, VIRGL_SHADER_FRAGMENT, fs_consts, 8);
    virgl_draw(&g->cmds, 0, 4, VIRGL_PRIM_TRIANGLE_STRIP);
}

/* Draw a layer with an additional screen-space clip.  The quad and its UVs
 * remain unchanged, so clipped pieces of a scaled app have no seams. */
static inline void gpucomp_draw_layer_scissored(int index, int x, int y,
                                                 int w, int h) {
    struct gpucomp *g = &gpucomp_state;
    int x2, y2;
    if (!g->ready || w <= 0 || h <= 0)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    x2 = x + w;
    y2 = y + h;
    if (x2 > g->screen_w) x2 = g->screen_w;
    if (y2 > g->screen_h) y2 = g->screen_h;
    if (x2 <= x || y2 <= y)
        return;
    /* Logical screen y=0 is the top, Gallium framebuffer y=0 the bottom. */
    virgl_set_scissor(&g->cmds, x, g->target_h - y2,
                      x2, g->target_h - y);
    gpucomp_draw_layer(index);
}

/* Render the normal desktop layer list into an offscreen scene target.  The
 * live screen occupies the top-left of a grow-only allocation, matching the
 * texture UV convention used everywhere else. */
static inline int gpucomp_target_begin(int index, int x, int y, int w, int h) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *l;
    int x2, y2;
    if (!g->ready || index < 1 || index >= GPUCOMP_MAX_LAYERS ||
        w <= 0 || h <= 0)
        return -1;
    l = &g->layers[index];
    if (!l->texture || !l->render_target)
        return -1;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    x2 = x + w;
    y2 = y + h;
    if (x2 > l->src_w) x2 = l->src_w;
    if (y2 > l->src_h) y2 = l->src_h;
    if (x2 <= x || y2 <= y)
        return 0;

    g->failed = 0;
    g->target_w = l->tex_w;
    g->target_h = l->tex_h;
    virgl_reset(&g->cmds);
    virgl_set_framebuffer(&g->cmds,
                          GPUCOMP_H_CANVAS_SURFACE_BASE + index);
    virgl_set_viewport(&g->cmds, l->tex_w, l->tex_h);
    virgl_set_scissor(&g->cmds, x, l->tex_h - y2, x2, l->tex_h - y);
    virgl_bind(&g->cmds, VIRGL_OBJECT_BLEND, VIRGL_H_BLEND);
    virgl_bind(&g->cmds, VIRGL_OBJECT_RASTERIZER, VIRGL_H_RASTERIZER);
    virgl_bind(&g->cmds, VIRGL_OBJECT_DSA, VIRGL_H_DSA);
    virgl_bind(&g->cmds, VIRGL_OBJECT_VERTEX_ELEMENTS,
               VIRGL_H_VERTEX_ELEMENTS);
    virgl_bind_shader(&g->cmds, VIRGL_H_VS, VIRGL_SHADER_VERTEX);
    virgl_bind_shader(&g->cmds, GPUCOMP_H_FS_ROUND,
                      VIRGL_SHADER_FRAGMENT);
    virgl_set_vertex_buffer(&g->cmds, gpucomp_vertex_id(), 4 * 4, 0);
    virgl_bind_sampler_state(&g->cmds, VIRGL_H_SAMPLER_STATE);
    return 0;
}

static inline int gpucomp_target_end(void) {
    struct gpucomp *g = &gpucomp_state;
    if (!g->ready || g->failed || g->cmds.overflow)
        return -1;
    return g->cmds.len ? virgl_flush(&g->cmds) : 0;
}

static inline int gpucomp_filter_pass(int destination, int source,
                                      float step_x, float step_y) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *dst, *src;
    uint32_t vs[4], fs[8];
    if (!g->ready || destination < 1 || source < 1 ||
        destination >= GPUCOMP_MAX_LAYERS || source >= GPUCOMP_MAX_LAYERS)
        return -1;
    dst = &g->layers[destination];
    src = &g->layers[source];
    if (!dst->texture || !dst->render_target || !src->texture)
        return -1;

    virgl_reset(&g->cmds);
    virgl_set_framebuffer(&g->cmds,
                          GPUCOMP_H_CANVAS_SURFACE_BASE + destination);
    virgl_set_viewport(&g->cmds, dst->tex_w, dst->tex_h);
    virgl_set_scissor(&g->cmds, 0, dst->tex_h - dst->src_h,
                      dst->src_w, dst->tex_h);
    virgl_bind(&g->cmds, VIRGL_OBJECT_BLEND, VIRGL_H_BLEND);
    virgl_bind(&g->cmds, VIRGL_OBJECT_RASTERIZER, VIRGL_H_RASTERIZER);
    virgl_bind(&g->cmds, VIRGL_OBJECT_DSA, VIRGL_H_DSA);
    virgl_bind(&g->cmds, VIRGL_OBJECT_VERTEX_ELEMENTS,
               VIRGL_H_VERTEX_ELEMENTS);
    virgl_bind_shader(&g->cmds, VIRGL_H_VS, VIRGL_SHADER_VERTEX);
    virgl_bind_shader(&g->cmds, GPUCOMP_H_FS_BLUR,
                      VIRGL_SHADER_FRAGMENT);
    virgl_set_vertex_buffer(&g->cmds, gpucomp_vertex_id(), 4 * 4, 0);
    virgl_bind_sampler_state(&g->cmds, VIRGL_H_SAMPLER_STATE);
    virgl_set_sampler_view(&g->cmds, GPUCOMP_H_VIEW_BASE + source);
    gpucomp_rect_consts_target(vs, 0, 0, dst->src_w, dst->src_h,
                               dst->tex_w, dst->tex_h);
    virgl_set_constants(&g->cmds, VIRGL_SHADER_VERTEX, vs, 4);
    fs[0] = gpucomp_f32(step_x);
    fs[1] = gpucomp_f32(step_y);
    fs[2] = gpucomp_f32(0.0f);
    fs[3] = gpucomp_f32(0.0f);
    fs[4] = gpucomp_f32((float)src->src_w / (float)src->tex_w);
    fs[5] = gpucomp_f32((float)src->src_h / (float)src->tex_h);
    fs[6] = gpucomp_f32(0.0f);
    fs[7] = gpucomp_f32(1.0f -
                        (float)src->src_h / (float)src->tex_h);
    virgl_set_constants(&g->cmds, VIRGL_SHADER_FRAGMENT, fs, 8);
    virgl_draw(&g->cmds, 0, 4, VIRGL_PRIM_TRIANGLE_STRIP);
    return virgl_flush(&g->cmds);
}

/* Downsample once, then perform a horizontal and vertical five-tap blur. */
static inline int gpucomp_blur_rebuild(int scene, int ping, int pong) {
    struct gpucomp_layer *a, *b;
    if (gpucomp_filter_pass(ping, scene, 0.0f, 0.0f) < 0)
        return -1;
    a = &gpucomp_state.layers[ping];
    if (gpucomp_filter_pass(pong, ping, 1.0f / (float)a->tex_w, 0.0f) < 0)
        return -1;
    b = &gpucomp_state.layers[pong];
    return gpucomp_filter_pass(ping, pong, 0.0f,
                               1.0f / (float)b->tex_h);
}

static inline int gpucomp_draw_acrylic(int blur_index, int x, int y,
                                       int w, int h, int radius,
                                       uint32_t tint, int tint_alpha,
                                       int clip_x, int clip_y,
                                       int clip_w, int clip_h) {
    struct gpucomp *g = &gpucomp_state;
    struct gpucomp_layer *blur;
    uint32_t vs[4], fs[12];
    int sx, sy, sx2, sy2;
    float live_u, live_v;
    if (!g->ready || blur_index < 1 || blur_index >= GPUCOMP_MAX_LAYERS ||
        w <= 0 || h <= 0 || clip_w <= 0 || clip_h <= 0)
        return -1;
    blur = &g->layers[blur_index];
    if (!blur->texture)
        return -1;
    sx = x > clip_x ? x : clip_x;
    sy = y > clip_y ? y : clip_y;
    sx2 = x + w < clip_x + clip_w ? x + w : clip_x + clip_w;
    sy2 = y + h < clip_y + clip_h ? y + h : clip_y + clip_h;
    if (sx2 <= sx || sy2 <= sy)
        return 0;
    if (radius < 0) radius = 0;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (tint_alpha < 0) tint_alpha = 0;
    if (tint_alpha > 255) tint_alpha = 255;
    if (gpucomp_canvas_room(g, 52) < 0)
        return -1;

    virgl_set_scissor(&g->cmds, sx, g->target_h - sy2,
                      sx2, g->target_h - sy);
    virgl_bind_shader(&g->cmds, GPUCOMP_H_FS_ACRYLIC,
                      VIRGL_SHADER_FRAGMENT);
    virgl_set_sampler_view(&g->cmds, GPUCOMP_H_VIEW_BASE + blur_index);
    gpucomp_rect_consts_target(vs, x, y, w, h, g->target_w, g->target_h);
    virgl_set_constants(&g->cmds, VIRGL_SHADER_VERTEX, vs, 4);
    fs[0] = gpucomp_f32((float)w * 0.5f);
    fs[1] = gpucomp_f32((float)h * 0.5f);
    fs[2] = gpucomp_f32((float)radius);
    fs[3] = gpucomp_f32((float)tint_alpha / 255.0f);
    gpucomp_color_constants(&fs[4], tint);
    live_u = (float)blur->src_w / (float)blur->tex_w;
    live_v = (float)blur->src_h / (float)blur->tex_h;
    fs[8] = gpucomp_f32((float)w / (float)g->screen_w * live_u);
    fs[9] = gpucomp_f32((float)h / (float)g->screen_h * live_v);
    fs[10] = gpucomp_f32((float)x / (float)g->screen_w * live_u);
    fs[11] = gpucomp_f32(1.0f -
                         (float)(y + h) / (float)g->screen_h * live_v);
    virgl_set_constants(&g->cmds, VIRGL_SHADER_FRAGMENT, fs, 12);
    virgl_draw(&g->cmds, 0, 4, VIRGL_PRIM_TRIANGLE_STRIP);
    return 0;
}

/* Acrylic is rendered from the completed offscreen scene.  The GUI keeps a
 * transparent overlay texture for text/icons, so the blur never feeds back
 * from the scanout and never has to read CPU-composited application pixels. */

static inline int gpucomp_end(int x, int y, int w, int h) {
    struct gpucomp *g = &gpucomp_state;
    if (!g->ready || g->failed)
        return -1;
    if (g->cmds.len && virgl_flush(&g->cmds) < 0)
        return -1;
    return gpu3d_present(x, y, w, h);
}

static inline void gpucomp_shutdown(void) {
    struct gpucomp *g = &gpucomp_state;
    if (g->ready)
        (void)gpu3d_scanout(0);
    for (int i = 1; i < GPUCOMP_MAX_LAYERS; i++) {
        if (g->layers[i].texture)
            (void)gpucomp_layer_release(i, 0);
    }
    if (g->font_texture) {
        if (g->ready) {
            virgl_reset(&g->cmds);
            virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SAMPLER_VIEW,
                                 GPUCOMP_H_FONT_VIEW);
            (void)virgl_flush(&g->cmds);
        }
        (void)gpu3d_resource_destroy(g->font_texture);
        g->font_texture = 0;
        g->font_pixels = 0;
        g->font_tex_w = 0;
        g->font_tex_h = 0;
        g->font_slot_count = 0;
        memset(g->glyph_codepoint, 0, sizeof(g->glyph_codepoint));
        memset(g->glyph_width, 0, sizeof(g->glyph_width));
    }
    if (g->ready) {
        virgl_reset(&g->cmds);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SURFACE,
                             VIRGL_H_SURFACE);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SHADER,
                             GPUCOMP_H_FS_GLYPH);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SHADER,
                             GPUCOMP_H_FS_ACRYLIC);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SHADER,
                             GPUCOMP_H_FS_SOLID);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SHADER,
                             GPUCOMP_H_FS_BLUR);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SHADER,
                             GPUCOMP_H_FS_ROUND);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SHADER, VIRGL_H_VS);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_VERTEX_ELEMENTS,
                             VIRGL_H_VERTEX_ELEMENTS);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_SAMPLER_STATE,
                             VIRGL_H_SAMPLER_STATE);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_DSA, VIRGL_H_DSA);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_RASTERIZER,
                             VIRGL_H_RASTERIZER);
        virgl_destroy_object(&g->cmds, VIRGL_OBJECT_BLEND, VIRGL_H_BLEND);
        (void)virgl_flush(&g->cmds);
    }
    if (g->layers[0].texture)
        (void)gpu3d_resource_destroy(g->layers[0].texture);
    memset(&g->layers[0], 0, sizeof(g->layers[0]));
    g->ready = 0;
}

#endif
