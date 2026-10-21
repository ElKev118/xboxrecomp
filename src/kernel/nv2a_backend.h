/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * ponytail: register-combiner state and texture stages 1-3 are not passed on:
 * a back end sees stage 0 and the diffuse colour. Extend Nv2aBatch when that
 * matters rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
    /* Origin of the clip rectangle (SET_SURFACE_CLIP_HORIZONTAL/VERTICAL low
     * halves), real pixels. Positions stay absolute: a title confining a draw
     * to a box (GR2's Equipment Setup weapon slots, 102x45 at (270,166))
     * moves the clip, not the coordinates, so the box is a scissor. */
    uint32_t clip_x, clip_y;
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    /* SET_POLY_OFFSET_FILL_ENABLE and SET_POLYGON_OFFSET_SCALE_FACTOR/BIAS:
     * z += bias + factor * (the triangle's depth slope), in depth units
     * (0..65535 for Z16) -- how a title keeps a shadow caster from
     * shadowing itself when its depth is compared with the shadow map. */
    uint32_t poly_offset_fill;
    float    poly_offset_factor, poly_offset_bias;
    /* The depth surface's format (1 Z16, 2 Z24S8) and float flag: where a
     * clear's SET_ZSTENCIL_CLEAR_VALUE keeps the depth (low 16 bits for Z16,
     * top 24 for Z24S8). */
    uint32_t zeta_format, z_float;
    /* SET_CLEAR_RECT_HORIZONTAL/VERTICAL: the inclusive rectangle a
     * CLEAR_SURFACE fills (xemu pgraph clear), real pixels. */
    uint32_t clear_x0, clear_x1, clear_y0, clear_y1;
} Nv2aRenderState;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
} Nv2aBatch;

/* ---- Programmable path (draw_xf) -------------------------------------
 *
 * What a GPU back end needs to draw a batch the way the title meant it,
 * rather than as stage-0-times-diffuse: unclipped vertices with every
 * output of the vertex stage, and the pixel stage's whole state -- four
 * texture stages, the texture-shader program and the register combiners.
 * A back end that sets draw_xf gets every vertex-program and fixed-function
 * batch this way (draw() still gets the screen-space path).
 */

/* One vertex as the vertex stage produced it. pos: x, y in surface pixels
 * (anti-aliasing scale included) and z in depth-buffer units, all already
 * divided by w; w is the clip-space w, so (pos.xyz before the divide) can be
 * rebuilt and the GPU clip the triangle itself. A vertex with w <= 0 is
 * behind the eye and still sent: clipping is the back end's job here. The
 * remaining outputs are as the program wrote them (oD0, oD1, oFog, oT0-3). */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog[4];
    float tex[4][4];
} Nv2aVertexXf;

/* One texture stage. Same fields as Nv2aTexture plus what the pixel stage
 * needs to address it. tex coordinates in Nv2aVertexXf are normalised for
 * swizzled and compressed formats and in texels for linear ones (normalised
 * flag), as the hardware takes them. */
typedef struct {
    Nv2aTexture t;
    uint32_t enabled;           /* stage has a texture bound and enabled */
    uint32_t normalised;        /* coordinates arrive in [0,1] */
    uint32_t cube;              /* six faces */
    uint32_t palette;           /* guest address of the CLUT (P8), else 0 */
    uint32_t format_word;       /* raw SET_TEXTURE_FORMAT */
    uint32_t control0;          /* raw SET_TEXTURE_CONTROL0 (alpha kill...) */
    uint32_t border_color;      /* SET_TEXTURE_BORDER_COLOR */
} Nv2aTexStage;

typedef struct {
    Nv2aTexStage stage[4];
    /* Register combiners and texture shader, raw (NV097 method values). */
    uint32_t color_icw[8], alpha_icw[8];
    uint32_t color_ocw[8], alpha_ocw[8];
    uint32_t factor0[8], factor1[8];
    uint32_t final0, final1;            /* SPECULAR_FOG_CW0/1 */
    uint32_t final_c0, final_c1;        /* SPECULAR_FOG_FACTOR0/1 */
    uint32_t control;                   /* COMBINER_CONTROL */
    uint32_t stage_program;             /* SHADER_STAGE_PROGRAM */
    uint32_t other_stage_input;         /* SHADER_OTHER_STAGE_INPUT */
    uint32_t dot_rgb_mapping;           /* DOT_RGBMAPPING */
    uint32_t clip_plane_mode;           /* SHADER_CLIP_PLANE_MODE */
    uint32_t fog_enable, fog_color;     /* FOG_ENABLE, FOG_COLOR */
    uint32_t shade_flat;                /* SHADE_MODE == FLAT */
    uint32_t zeta_format;               /* 0 none, 1 Z16, 2 Z24S8 */
    uint32_t z_float;                   /* CONTROL0 bit 12: float depth */
    float    zmax;                      /* largest depth value (65535 / 16777215) */
    /* The combiners above are the title's (it has set SET_COMBINER_* at
     * least once); 0: draw texture 0 times diffuse, as the CPU path does. */
    uint32_t rc_seen;
    /* Fog factor from oFog.x: SET_FOG_MODE (GL enums) and SET_FOG_PARAMS
     * 0/1 (bias, scale), as the executor's own fog_factor() uses them. */
    uint32_t fog_mode;
    float    fog_param[2];
    /* SET_SURFACE_FORMAT colour field: 4 X8R8G8B8_Z8 / 5 _O8 (alpha
     * written as 0 / 1), 8 A8R8G8B8, 3 R5G6B5, 1/2 X1R5G5B5 ... */
    uint32_t color_format;
    /* SET_SHADOW_DEPTH_FUNC (0 never .. 7 always): how a stage that samples
     * a depth texture compares the stored depth with the coordinate's z. */
    uint32_t shadow_func;
} Nv2aPixelState;

typedef struct {
    const Nv2aVertexXf    *vertices;    /* triangle list, count % 3 == 0 */
    uint32_t               count;
    const Nv2aRenderState *state;
    const Nv2aPixelState  *pixel;
} Nv2aBatchXf;

/* The programmable path with the vertex program left to the back end
 * (draw_vsh): the program's inputs, as floats, for each distinct vertex the
 * batch uses, an index list of triangles over them (strips and fans already
 * expanded, every triangle wound like the first), the program and its
 * constants. The back end compiles the program (nv2a_vsh_glsl.h) and runs it
 * per vertex; the outputs mean what Nv2aVertexXf's fields do -- oPos in
 * surface pixels of the program's viewport (multiply x, y by aa_sx/aa_sy),
 * z in depth units, both divided by w; oPos.w == +0 means 1. */
typedef struct {
    const float           *inputs;      /* nverts * ninputs vec4s          */
    uint32_t               nverts;
    uint32_t               ninputs;     /* attributes packed per vertex     */
    uint8_t                input_reg[16]; /* v register of each packed slot */
    const uint32_t        *indices;     /* triangle list over the vertices */
    uint32_t               nindices;
    const uint32_t       (*program)[4]; /* program memory                  */
    uint32_t               nslots, start;
    uint64_t               program_hash;
    const float          (*constants)[4]; /* c[0..191]                     */
    float                  aa_sx, aa_sy;
    const Nv2aRenderState *state;
    const Nv2aPixelState  *pixel;
} Nv2aBatchVsh;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    /* Optional (NULL: not provided). draw_xf: see Nv2aBatchXf above.
     * blit: NV09F image blit (SRCCOPY) between guest addresses, bpp bytes
     * per pixel; returns 1 if the back end did it (its own copy of either
     * surface), 0 to let the executor copy guest memory. present: the
     * surface the title has just flipped to, for a back end that owns the
     * display -- called before flip(). */
    void (*draw_xf)(const Nv2aSurface *s, const Nv2aBatchXf *b);
    int  (*blit)(uint32_t src_va, uint32_t src_pitch, uint32_t dst_va,
                 uint32_t dst_pitch, uint32_t sx, uint32_t sy, uint32_t dx,
                 uint32_t dy, uint32_t w, uint32_t h, uint32_t bpp);
    void (*present)(uint32_t surface_va, uint32_t pitch, uint32_t width,
                    uint32_t height, uint32_t aa_sx, uint32_t aa_sy);
    /* Optional: run vertex programs on the GPU (Nv2aBatchVsh). Returns 1 if
     * drawn; 0 sends the batch down the CPU path (draw_xf) instead. */
    int  (*draw_vsh)(const Nv2aSurface *s, const Nv2aBatchVsh *b);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#endif /* NV2A_BACKEND_H */
