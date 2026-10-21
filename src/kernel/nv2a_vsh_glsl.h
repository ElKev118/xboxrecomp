/**
 * NV2A vertex program -> GLSL, for a GPU back end.
 *
 * The interpreter (nv2a_vsh_interp.c) runs a title's vertex program on the
 * CPU one vertex at a time; on Ghost Recon 2 that was ~80% of the pushbuffer
 * thread. This turns the same program into GLSL source that a back end
 * compiles once and runs on the GPU. The semantics are the interpreter's,
 * rule for rule (it is the reference that was checked against real titles):
 * every source is read before either unit writes, an ILU op paired with a
 * MAC op writes R1, temps past R11 other than R12 (oPos) read R0, constant
 * indices out of range read c[0], RCC clamps, ARL floors x + 0.001.
 *
 * The generated text is a function body fragment. It expects, declared by
 * the back end around it:
 *     vec4 v[16];        the inputs (unused ones may hold anything)
 *     vec4 cst[192];     the constants (a uniform block member)
 * and it declares and fills, for the back end's epilogue to read:
 *     vec4 oPos;         what the program wrote to oPos
 *     vec4 o[13];        output registers: 3 D0, 4 D1, 5 fog, 9..12 T0..T3
 *                        (colours default to w = 1, texcoords to (0,0,0,1))
 * Portable C; no graphics API.
 */
#ifndef XBOXRECOMP_NV2A_VSH_GLSL_H
#define XBOXRECOMP_NV2A_VSH_GLSL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t inputs;        /* bit i: the program reads v[i] */
    int      writes_consts; /* the program writes c[] (needs CXT_WRITE: CPU) */
    uint32_t ninstr;        /* instructions up to and including FINAL */
    uint64_t hash;          /* of those instructions, for a shader cache */
} Nv2aVshInfo;

/* Scan the program from `start`. Returns 0 when no FINAL instruction is
 * found within program memory (nothing to run). */
int nv2a_vsh_scan(const uint32_t (*prog)[4], uint32_t nslots, uint32_t start,
                  Nv2aVshInfo *info);

/* Helper functions the body calls (vsh_rcp, vsh_rcc, vsh_exp, vsh_log,
 * vsh_lit, vsh_ci); declare once at global scope, before main. */
const char *nv2a_vsh_glsl_prelude(void);

/* Write the body for the program at `start` into buf (NUL-terminated).
 * Returns the length, or 0 if it did not fit or there is no program. */
size_t nv2a_vsh_glsl(const uint32_t (*prog)[4], uint32_t nslots, uint32_t start,
                     char *buf, size_t size);

#endif
