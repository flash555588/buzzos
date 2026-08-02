/*
 * virgl pipeline test: textured quads with alpha blending on the host GPU.
 *
 * Proves the part a CLEAR cannot: that TGSI shaders compile on the host, that
 * vertex/texture resources upload correctly, and that hardware alpha blending
 * and bilinear filtering work.  These are exactly the primitives the the theme
 * compositor needs (window surfaces as textures, translucent acrylic panels).
 *
 * Run from the shell:  gputest
 */

#include "libc.h"
#include "virgl.h"

enum {
    TEX_W = 64,
    TEX_H = 64,
    QUAD_VERTS = 4,
    FLOATS_PER_VERT = 4, /* x, y, u, v */
};

/* Pass-through vertex shader: clip-space position + texcoord.
 * Written in TGSI, which virglrenderer translates to GLSL host-side.
 * IMM[0] must be declared explicitly -- an undeclared immediate makes the
 * host-side parse fail silently, since SUBMIT_3D still returns OK. */
static const char *VS_TGSI =
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

/* Sample the texture and modulate by a constant colour (rgb + alpha). */
static const char *FS_TGSI =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL CONST[0..0]\n"
    "DCL TEMP[0]\n"
    "  0: TEX TEMP[0], IN[0], SAMP[0], 2D\n"
    "  1: MUL OUT[0], TEMP[0], CONST[0]\n"
    "  2: END\n";

/* Solid-colour fragment shader, no texture -- used for the blended overlay.
 * Declares no inputs: an unused IN[] declaration is a parse risk and buys
 * nothing here. */
static const char *FS_SOLID_TGSI =
    "FRAG\n"
    "DCL OUT[0], COLOR\n"
    "DCL CONST[0..0]\n"
    "  0: MOV OUT[0], CONST[0]\n"
    "  1: END\n";

static struct virgl_cmdbuf cmds;

/* Fill a checkerboard so bilinear filtering and orientation are obvious.
 *
 * Alpha must be 0xFF: the texture is sampled with SRC_ALPHA blending, so the
 * 0x00RRGGBB convention used for opaque framebuffer pixels elsewhere in the
 * system would make every texel fully transparent here. */
static void fill_test_texture(uint32_t *pixels) {
    for (int y = 0; y < TEX_H; y++) {
        for (int x = 0; x < TEX_W; x++) {
            int cell = ((x / 8) + (y / 8)) & 1;
            uint32_t colour = cell ? 0xFFFF8800u : 0xFF104080u;
            /* Mark the top-left corner so a vertical flip is visible. */
            if (x < 10 && y < 10)
                colour = 0xFFFFFFFFu;
            pixels[y * TEX_W + x] = colour;
        }
    }
}

/* Quad in normalized [0,1] space; the vertex shader maps it to clip space via
 * CONST[0] = (scale_x, scale_y, offset_x, offset_y).
 *
 * Orientation lives here and nowhere else: position y grows downward (screen
 * order) while texcoord v is emitted flipped, so texture row 0 draws at the
 * top.  The viewport stays in plain GL orientation.  Earlier attempts flipped
 * in the viewport *and* the rect math, which cancelled out. */
static void fill_quad(float *verts) {
    static const float corners[QUAD_VERTS][4] = {
        /*  x     y     u     v   */
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 1.0f, 0.0f},
    };
    for (int i = 0; i < QUAD_VERTS; i++) {
        verts[i * FLOATS_PER_VERT + 0] = corners[i][0];
        verts[i * FLOATS_PER_VERT + 1] = corners[i][1];
        verts[i * FLOATS_PER_VERT + 2] = corners[i][2];
        verts[i * FLOATS_PER_VERT + 3] = corners[i][3];
    }
}

/* Map a rect in top-left pixel coordinates to the MAD constants the vertex
 * shader expects.  Screen y grows downward, clip space y grows upward, so the
 * scale is negated and the offset measured from the top edge. */
static void rect_constants(uint32_t *out, int x, int y, int w, int h,
                           int screen_w, int screen_h) {
    float sx = (float)w * 2.0f / (float)screen_w;
    float sy = -((float)h * 2.0f / (float)screen_h);
    float ox = (float)x * 2.0f / (float)screen_w - 1.0f;
    float oy = 1.0f - (float)y * 2.0f / (float)screen_h;
    out[0] = virgl_f32(sx);
    out[1] = virgl_f32(sy);
    out[2] = virgl_f32(ox);
    out[3] = virgl_f32(oy);
}

int main(void) {
    struct gpu3d_caps caps;
    struct gpu3d_resource texture, vertices;
    uint32_t constants[4];
    int screen_w, screen_h;

    if (gpu3d_info(&caps) < 0 || !caps.available) {
        printf("virgl unavailable: run QEMU with -device virtio-vga-gl\n");
        return 1;
    }
    printf("virgl %ux%u, rt=%u, cmd cap=%u bytes\n",
           caps.width, caps.height, caps.scanout_resource,
           caps.command_capacity);
    screen_w = (int)caps.width;
    screen_h = (int)caps.height;

    if (gfx_acquire_display() < 0) {
        printf("could not acquire display\n");
        return 1;
    }

    if (gpu3d_resource_create(VIRGL_TARGET_TEXTURE_2D,
                              VIRGL_FORMAT_B8G8R8A8_UNORM,
                              VIRGL_BIND_SAMPLER_VIEW, TEX_W, TEX_H,
                              &texture) < 0) {
        printf("texture create failed\n");
        gfx_release_display();
        return 1;
    }
    fill_test_texture(texture.pixels);
    if (gpu3d_upload(texture.id, 0, 0, TEX_W, TEX_H) < 0) {
        printf("texture upload failed\n");
        gfx_release_display();
        return 1;
    }

    uint32_t vertex_bytes = QUAD_VERTS * FLOATS_PER_VERT * 4u;
    if (gpu3d_resource_create(VIRGL_TARGET_BUFFER,
                              VIRGL_FORMAT_R32G32_FLOAT,
                              VIRGL_BIND_VERTEX_BUFFER, vertex_bytes, 1,
                              &vertices) < 0) {
        printf("vertex buffer create failed\n");
        gfx_release_display();
        return 1;
    }
    fill_quad((float *)vertices.pixels);
    if (gpu3d_upload(vertices.id, 0, 0, (int)vertex_bytes, 1) < 0) {
        printf("vertex upload failed\n");
        gfx_release_display();
        return 1;
    }

    /* Build all pipeline objects once. */
    virgl_reset(&cmds);
    virgl_create_surface(&cmds, VIRGL_H_SURFACE, caps.scanout_resource,
                         VIRGL_FORMAT_B8G8R8A8_UNORM);
    virgl_create_blend(&cmds, VIRGL_H_BLEND, 1);
    virgl_create_rasterizer(&cmds, VIRGL_H_RASTERIZER);
    virgl_create_dsa(&cmds, VIRGL_H_DSA);
    virgl_create_sampler_state(&cmds, VIRGL_H_SAMPLER_STATE, 1);
    virgl_create_sampler_view(&cmds, VIRGL_H_SAMPLER_VIEW_BASE, texture.id,
                              VIRGL_FORMAT_B8G8R8A8_UNORM);
    virgl_create_vertex_elements(&cmds, VIRGL_H_VERTEX_ELEMENTS);
    virgl_create_shader(&cmds, VIRGL_H_VS, VIRGL_SHADER_VERTEX, VS_TGSI);
    virgl_create_shader(&cmds, VIRGL_H_FS, VIRGL_SHADER_FRAGMENT, FS_TGSI);
    virgl_create_shader(&cmds, VIRGL_H_FS_SOLID, VIRGL_SHADER_FRAGMENT,
                        FS_SOLID_TGSI);
    if (virgl_flush(&cmds) < 0) {
        printf("pipeline object creation failed\n");
        gfx_release_display();
        return 1;
    }
    printf("pipeline objects created (shaders compiled host-side)\n");

    /* Bind the pipeline and draw. */
    virgl_reset(&cmds);
    virgl_set_framebuffer(&cmds, VIRGL_H_SURFACE);
    virgl_set_viewport(&cmds, screen_w, screen_h);
    virgl_bind(&cmds, VIRGL_OBJECT_BLEND, VIRGL_H_BLEND);
    virgl_bind(&cmds, VIRGL_OBJECT_RASTERIZER, VIRGL_H_RASTERIZER);
    virgl_bind(&cmds, VIRGL_OBJECT_DSA, VIRGL_H_DSA);
    virgl_bind(&cmds, VIRGL_OBJECT_VERTEX_ELEMENTS, VIRGL_H_VERTEX_ELEMENTS);
    virgl_bind_shader(&cmds, VIRGL_H_VS, VIRGL_SHADER_VERTEX);
    virgl_bind_shader(&cmds, VIRGL_H_FS, VIRGL_SHADER_FRAGMENT);
    virgl_set_vertex_buffer(&cmds, vertices.id, FLOATS_PER_VERT * 4u, 0);
    virgl_set_sampler_view(&cmds, VIRGL_H_SAMPLER_VIEW_BASE);
    virgl_bind_sampler_state(&cmds, VIRGL_H_SAMPLER_STATE);
    virgl_clear(&cmds, 0.09f, 0.11f, 0.16f, 1.0f);

    /* Opaque texture, upscaled 64x64 -> 400x300.  If bilinear filtering is
     * live the checkerboard edges are smooth, not blocky. */
    rect_constants(constants, 80, 80, 400, 300, screen_w, screen_h);
    virgl_set_constants(&cmds, VIRGL_SHADER_VERTEX, constants, 4);
    constants[0] = virgl_f32(1.0f);
    constants[1] = virgl_f32(1.0f);
    constants[2] = virgl_f32(1.0f);
    constants[3] = virgl_f32(1.0f);
    virgl_set_constants(&cmds, VIRGL_SHADER_FRAGMENT, constants, 4);
    virgl_draw(&cmds, 0, QUAD_VERTS, VIRGL_PRIM_TRIANGLE_STRIP);

    /* Translucent red bar across the checkerboard, drawn with the solid-colour
     * shader.  A second copy of the same texture would blend into itself and
     * look unchanged, so the overlay uses a contrasting colour instead: where
     * it crosses the checkerboard the pattern must stay visible through it. */
    virgl_bind_shader(&cmds, VIRGL_H_FS_SOLID, VIRGL_SHADER_FRAGMENT);
    rect_constants(constants, 180, 40, 160, 420, screen_w, screen_h);
    virgl_set_constants(&cmds, VIRGL_SHADER_VERTEX, constants, 4);
    constants[0] = virgl_f32(1.0f);
    constants[1] = virgl_f32(0.15f);
    constants[2] = virgl_f32(0.15f);
    constants[3] = virgl_f32(0.5f);
    virgl_set_constants(&cmds, VIRGL_SHADER_FRAGMENT, constants, 4);
    virgl_draw(&cmds, 0, QUAD_VERTS, VIRGL_PRIM_TRIANGLE_STRIP);

    /* Opaque green reference bar: same colour path, alpha 1.0.  Comparing the
     * two bars separates "blending works" from "the colour is just dark". */
    rect_constants(constants, 380, 40, 60, 420, screen_w, screen_h);
    virgl_set_constants(&cmds, VIRGL_SHADER_VERTEX, constants, 4);
    constants[0] = virgl_f32(0.2f);
    constants[1] = virgl_f32(0.9f);
    constants[2] = virgl_f32(0.3f);
    constants[3] = virgl_f32(1.0f);
    virgl_set_constants(&cmds, VIRGL_SHADER_FRAGMENT, constants, 4);
    virgl_draw(&cmds, 0, QUAD_VERTS, VIRGL_PRIM_TRIANGLE_STRIP);

    if (virgl_flush(&cmds) < 0) {
        printf("draw submit failed\n");
        gfx_release_display();
        return 1;
    }

    if (gpu3d_scanout(1) < 0 ||
        gpu3d_present(0, 0, screen_w, screen_h) < 0) {
        printf("present failed\n");
        gfx_release_display();
        return 1;
    }
    printf("drew 2 quads (1 opaque, 1 at 50%% alpha); 5s\n");
    sleep_ms(5000);

    gpu3d_scanout(0);
    gpu3d_resource_destroy(texture.id);
    gpu3d_resource_destroy(vertices.id);
    gfx_release_display();
    printf("gputest done\n");
    return 0;
}
