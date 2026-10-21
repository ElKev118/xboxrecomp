/**
 * NV2A vertex program -> GLSL. See nv2a_vsh_glsl.h; the encoding table is at
 * the top of nv2a_vsh_interp.c, and every rule below mirrors nv2a_vsh_run.
 */
#include "nv2a_vsh_glsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

const char *nv2a_vsh_glsl_prelude(void)
{
    /* The interpreter's ILU ops and relative constant index, as functions. */
    return
        "#define VSH_INF uintBitsToFloat(0x7F800000u)\n"
        "int vsh_ci(int i) { return (i < 0 || i >= 192) ? 0 : i; }\n"
        "vec4 vsh_rcp(float x) { return vec4(x == 0.0 ? VSH_INF : 1.0 / x); }\n"
        "vec4 vsh_rcc(float x) {\n"
        "  float f = 1.0 / x;\n"
        "  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
        "  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
        "  return vec4(f);\n"
        "}\n"
        "vec4 vsh_exp(float x) {\n"
        "  float fl = floor(x);\n"
        "  return vec4(exp2(fl), x - fl, exp2(x), 1.0);\n"
        "}\n"
        "vec4 vsh_log(float x) {\n"
        "  float ax = abs(x);\n"
        "  if (ax == 0.0) return vec4(-VSH_INF, 1.0, -VSH_INF, 1.0);\n"
        "  float e = floor(log2(ax));\n"
        "  return vec4(e, ax / exp2(e), log2(ax), 1.0);\n"
        "}\n"
        "vec4 vsh_lit(vec4 c) {\n"
        "  float nl = max(c.x, 0.0), nh = max(c.y, 0.0);\n"
        "  float p = clamp(c.w, -127.9961, 127.9961);\n"
        "  float sp = p == 0.0 ? 1.0 : pow(nh, p);\n"
        "  return vec4(1.0, nl, c.x > 0.0 ? sp : 0.0, 1.0);\n"
        "}\n";
}

int nv2a_vsh_scan(const uint32_t (*prog)[4], uint32_t nslots, uint32_t start,
                  Nv2aVshInfo *info)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t s;

    memset(info, 0, sizeof *info);
    for (s = start; s < nslots; s++) {
        const uint32_t *ins = prog[s];
        int k;
        for (k = 0; k < 4; k++)
            h = (h ^ ins[k]) * 1099511628211ull;
        info->ninstr++;
        /* Inputs: a source reads v[] when its mux is 2; all three share the
         * input index field. */
        if (field(ins, 2, 26, 2) == 2 || field(ins, 2, 11, 2) == 2
            || field(ins, 3, 28, 2) == 2)
            info->inputs |= 1u << field(ins, 1, 9, 4);
        if (field(ins, 3, 12, 4) && !field(ins, 3, 11, 1))
            info->writes_consts = 1;
        if (field(ins, 3, 0, 1)) {
            info->hash = h ^ start;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    char  *buf;
    size_t size, len;
    int    overflow;
} Out;

static void put(Out *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (o->overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->buf + o->len, o->size - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->size - o->len) {
        o->overflow = 1;
        return;
    }
    o->len += (size_t)n;
}

/* One source operand as a GLSL expression (swizzled, maybe negated). */
static void src(Out *o, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    put(o, "%s(", neg ? "-" : "");
    if (mux == 1) {
        if (reg == 12)
            put(o, "oPos");
        else
            put(o, "R[%u]", reg < 12 ? reg : 0u);
    } else if (mux == 2) {
        put(o, "v[%u]", field(ins, 1, 9, 4));
    } else {
        uint32_t ci = field(ins, 1, 13, 8);
        if (field(ins, 3, 1, 1))
            put(o, "cst[vsh_ci(%u + a0)]", ci);
        else
            put(o, "cst[%u]", ci < 192 ? ci : 0u);
    }
    put(o, ").%c%c%c%c", sw[(swz >> 6) & 3], sw[(swz >> 4) & 3],
        sw[(swz >> 2) & 3], sw[swz & 3]);
}

static void masked(Out *o, const char *dst, const char *val, uint32_t mask)
{
    if (mask & 8) put(o, "    %s.x = %s.x;\n", dst, val);
    if (mask & 4) put(o, "    %s.y = %s.y;\n", dst, val);
    if (mask & 2) put(o, "    %s.z = %s.z;\n", dst, val);
    if (mask & 1) put(o, "    %s.w = %s.w;\n", dst, val);
}

static void temp_name(char *b, size_t n, uint32_t t)
{
    if (t == 12)
        snprintf(b, n, "oPos");
    else if (t < 12)
        snprintf(b, n, "R[%u]", t);
    else
        b[0] = 0;
}

size_t nv2a_vsh_glsl(const uint32_t (*prog)[4], uint32_t nslots, uint32_t start,
                     char *buf, size_t size)
{
    Out o = { buf, size, 0, 0 };
    uint32_t s;

    if (!size)
        return 0;
    buf[0] = 0;
    put(&o, "  vec4 R[12];\n  for (int k = 0; k < 12; k++) R[k] = vec4(0.0);\n"
            "  vec4 oPos = vec4(0.0);\n  vec4 o[13];\n"
            "  for (int k = 0; k < 13; k++) o[k] = vec4(0.0);\n"
            "  o[3].w = 1.0; o[4].w = 1.0;\n"
            "  o[9].w = 1.0; o[10].w = 1.0; o[11].w = 1.0; o[12].w = 1.0;\n"
            "  int a0 = 0;\n");
    for (s = start; s < nslots; s++) {
        const uint32_t *ins = prog[s];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
        uint32_t tdst = field(ins, 3, 20, 4);
        uint32_t omask = field(ins, 3, 12, 4), oaddr = field(ins, 3, 3, 8);
        char d[16];

        put(&o, "  {\n    vec4 A = "); src(&o, ins, 0);
        put(&o, ";\n    vec4 B = "); src(&o, ins, 1);
        put(&o, ";\n    vec4 C = "); src(&o, ins, 2);
        put(&o, ";\n    vec4 m = vec4(0.0), i = vec4(0.0);\n");
        switch (mac) {
        case 0:  break;
        case 2:  put(&o, "    m = A * B;\n"); break;
        case 3:  put(&o, "    m = A + C;\n"); break;
        case 4:  put(&o, "    m = A * B + C;\n"); break;
        case 5:  put(&o, "    m = vec4(dot(A.xyz, B.xyz));\n"); break;
        case 6:  put(&o, "    m = vec4(dot(A.xyz, B.xyz) + B.w);\n"); break;
        case 7:  put(&o, "    m = vec4(dot(A, B));\n"); break;
        case 8:  put(&o, "    m = vec4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9:  put(&o, "    m = mix(B, A, lessThan(A, B));\n"); break;
        case 10: put(&o, "    m = mix(B, A, greaterThanEqual(A, B));\n"); break;
        case 11: put(&o, "    m = vec4(lessThan(A, B));\n"); break;
        case 12: put(&o, "    m = vec4(greaterThanEqual(A, B));\n"); break;
        case 13: put(&o, "    m = A;\n"); break;                  /* arl, below */
        default: put(&o, "    m = A;\n"); break;                  /* mov, 14, 15 */
        }
        switch (ilu) {
        case 0:  break;
        case 2:  put(&o, "    i = vsh_rcp(C.x);\n"); break;
        case 3:  put(&o, "    i = vsh_rcc(C.x);\n"); break;
        case 4:  put(&o, "    i = vec4(1.0 / sqrt(abs(C.x)));\n"); break;
        case 5:  put(&o, "    i = vsh_exp(C.x);\n"); break;
        case 6:  put(&o, "    i = vsh_log(C.x);\n"); break;
        case 7:  put(&o, "    i = vsh_lit(C);\n"); break;
        default: put(&o, "    i = C;\n"); break;                  /* mov */
        }
        if (mac == 13)
            put(&o, "    a0 = int(floor(A.x + 0.001));\n");
        if (mac && mac != 13 && mac_mask) {
            temp_name(d, sizeof d, tdst);
            if (d[0])
                masked(&o, d, "m", mac_mask);
        }
        if (ilu && ilu_mask) {
            temp_name(d, sizeof d, mac ? 1u : tdst);
            if (d[0])
                masked(&o, d, "i", ilu_mask);
        }
        if (omask && field(ins, 3, 11, 1)) {      /* writes to c[]: not here */
            const char *v = field(ins, 3, 2, 1) ? "i" : "m";
            if (oaddr == 0)
                masked(&o, "oPos", v, omask);
            else if (oaddr < 13) {
                snprintf(d, sizeof d, "o[%u]", oaddr);
                masked(&o, d, v, omask);
            }
        }
        put(&o, "  }\n");
        if (field(ins, 3, 0, 1))
            return o.overflow ? 0 : o.len;
    }
    return 0;
}
