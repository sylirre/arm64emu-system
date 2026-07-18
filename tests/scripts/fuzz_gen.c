/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Cross-engine differential fuzz generator: emits a flat .bin (load at
 * 0x40000000) of pseudo-random A64 straight-line blocks weighted toward the
 * JIT's *inlined* surface (ALU, bitfield, csel/ccmp, mul/div, loads/stores in
 * every addressing mode, exclusives, scalar/vector FP) plus short forward
 * branches to exercise block chaining and lazy-NZCV materialization at exits.
 * The same image must produce a byte-identical cpu_dump under the
 * interpreter, -pd and -jit (tests/run_fuzz_engines.sh).
 *
 * Constraints that keep runs fault-free and device-free (so byte-identical
 * state is *required*, with no tick-cadence escape hatch): no SP writes, no
 * system instructions except the final MRS FPSR, and every memory access
 * lands inside an in-image scratch region. Reserved registers: x26 = data
 * pool base, x27 = pre-masked register offset, x28 = scratch base (never
 * destinations); x25 is a writeback/exclusive base re-established before
 * each use. cpu_dump prints only GPRs, so the epilogue folds all V registers
 * and FPSR (forcing the lazy fpsr_sync fold) into x0/x1/x2 before HLT #0.
 *
 * A bad encoding cannot fail silently: an UNDEF vectors to VBAR=0 whose
 * memory is not a valid handler, the run never reaches HLT, and the runner
 * flags the seed as a generator bug (maxinsn reached).
 *
 * Usage: fuzz_gen SEED [NINSNS] > f.bin   (NINSNS 0/absent = seed-derived)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOAD_BASE   0x40000000u
#define POOL_OFF    0x4000u          /* 4 KB of random data for V-reg seeding */
#define POOL_SIZE   0x1000u
#define SCRATCH_OFF (POOL_OFF + POOL_SIZE)
#define SCRATCH_SIZE 0x8000u         /* 32 KB, random-initialized             */
#define BIN_SIZE    (SCRATCH_OFF + SCRATCH_SIZE)
#define MAX_WORDS   (POOL_OFF / 4)

static uint32_t code[MAX_WORDS];
static unsigned nw;
static uint64_t rngs;

static uint64_t rnd(void) {          /* xorshift64* */
    rngs ^= rngs >> 12; rngs ^= rngs << 25; rngs ^= rngs >> 27;
    return rngs * 0x2545F4914F6CDD1DULL;
}
static unsigned rr(unsigned n) { return (unsigned)(rnd() % n); }

static void put(uint32_t w) {
    if (nw >= MAX_WORDS) { fprintf(stderr, "code overflow\n"); exit(1); }
    code[nw++] = w;
}

/* Destinations: never the reserved bases x26-x28, never 31 (SP in the
 * immediate/extended forms). Sources may be anything but 31. */
static unsigned rd(void) { unsigned r; do r = rr(31); while (r >= 26 && r <= 28); return r; }
static unsigned rs(void) { return rr(31); }

static void movzk64(unsigned reg, uint64_t val) {  /* MOVZ + up to 3 MOVK */
    put(0xD2800000u | ((uint32_t)(val & 0xFFFF) << 5) | reg);
    for (int hw = 1; hw < 4; hw++) {
        uint32_t imm = (val >> (16 * hw)) & 0xFFFF;
        if (imm) put(0xF2800000u | ((uint32_t)hw << 21) | (imm << 5) | reg);
    }
}

/* ---- one random instruction (or a small fixed multi-insn unit) ---- */

static void unit(int allow_branch);

static void u_addsub_imm(void) {
    unsigned sf = rr(2), op = rr(2), S = rr(2), sh = rr(2);
    put(0x11000000u | sf << 31 | op << 30 | S << 29 | sh << 22 |
        rr(0x1000) << 10 | rs() << 5 | rd());
}
static void u_logic_imm(void) {
    unsigned sf = rr(2), opc = rr(4), N, immr, imms, k;
    if (sf) { N = rr(2); } else { N = 0; }
    if (N) { k = 1 + rr(63); immr = rr(64); imms = k - 1; }          /* e=64 */
    else   { k = 1 + rr(31); immr = rr(32); imms = k - 1; }          /* e=32 */
    put(0x12000000u | sf << 31 | opc << 29 | N << 22 | immr << 16 |
        imms << 10 | rs() << 5 | rd());
}
static void u_movewide(void) {
    static const uint32_t op[3] = { 0x12800000u, 0x52800000u, 0x72800000u };
    unsigned sf = rr(2), hw = rr(sf ? 4 : 2);
    put(op[rr(3)] | sf << 31 | hw << 21 | rr(0x10000) << 5 | rd());
}
static void u_bitfield(void) {
    unsigned sf = rr(2), opc = rr(3), w = sf ? 64 : 32;
    put(0x13000000u | sf << 31 | opc << 29 | sf << 22 | rr(w) << 16 |
        rr(w) << 10 | rs() << 5 | rd());
}
static void u_extr(void) {
    unsigned sf = rr(2);
    put(0x13800000u | sf << 31 | sf << 22 | rs() << 16 |
        rr(sf ? 64 : 32) << 10 | rs() << 5 | rd());
}
static void u_logic_reg(void) {
    unsigned sf = rr(2);
    put(0x0A000000u | sf << 31 | rr(4) << 29 | rr(4) << 22 | rr(2) << 21 |
        rs() << 16 | rr(sf ? 64 : 32) << 10 | rs() << 5 | rd());
}
static void u_addsub_shifted(void) {
    unsigned sf = rr(2);
    put(0x0B000000u | sf << 31 | rr(2) << 30 | rr(2) << 29 | rr(3) << 22 |
        rs() << 16 | rr(sf ? 64 : 32) << 10 | rs() << 5 | rd());
}
static void u_addsub_ext(void) {
    unsigned r; do r = rr(31); while (r >= 26 && r <= 28);  /* Rn=31 is SP */
    put(0x0B200000u | rr(2) << 31 | rr(2) << 30 | rr(2) << 29 | rs() << 16 |
        rr(8) << 13 | rr(5) << 10 | r << 5 | rd());
}
static void u_adcsbc(void) {
    put(0x1A000000u | rr(2) << 31 | rr(2) << 30 | rr(2) << 29 | rs() << 16 |
        rs() << 5 | rd());
}
static void u_csel(void) {
    put(0x1A800000u | rr(2) << 31 | rr(2) << 30 | rs() << 16 | rr(14) << 12 |
        rr(2) << 10 | rs() << 5 | rd());
}
static void u_ccmp(void) {
    unsigned base = rr(2) ? 0x7A400000u : 0x3A400000u;   /* CCMP / CCMN */
    put(base | rr(2) << 31 | (rr(2) ? 0x800u : 0u) | rr(32) << 16 |
        rr(14) << 12 | rs() << 5 | rr(16));
}
static void u_dp3(void) {
    static const struct { unsigned op31, o0, sf_only; } f[8] = {
        {0,0,0},{0,1,0},{1,0,1},{1,1,1},{2,0,1},{5,0,1},{5,1,1},{6,0,1} };
    unsigned i = rr(8), sf = f[i].sf_only ? 1 : rr(2);
    unsigned ra = (f[i].op31 == 2 || f[i].op31 == 6) ? 31 : rs();
    put(0x1B000000u | sf << 31 | f[i].op31 << 21 | rs() << 16 |
        f[i].o0 << 15 | ra << 10 | rs() << 5 | rd());
}
static void u_dp2(void) {
    static const unsigned opc[6] = { 2, 3, 8, 9, 10, 11 };
    put(0x1AC00000u | rr(2) << 31 | rs() << 16 | opc[rr(6)] << 10 |
        rs() << 5 | rd());
}
static void u_dp1(void) {
    unsigned sf = rr(2), opc;
    if (sf) { static const unsigned o[6] = {0,1,2,3,4,5}; opc = o[rr(6)]; }
    else    { static const unsigned o[5] = {0,1,2,4,5};   opc = o[rr(5)]; }
    put(0x5AC00000u | sf << 31 | opc << 10 | rs() << 5 | rd());
}
static void u_adr(void) {
    unsigned imm = rr(0x1000);       /* +-4KB region, value-only use */
    put((rr(2) ? 0x90000000u : 0x10000000u) | (imm & 3) << 29 |
        (imm >> 2) << 5 | rd());
}
static void u_ldst_imm(void) {
    unsigned size = rr(4), opc;
    if (size == 3) opc = rr(2);                    /* opc=10 is PRFM: avoid */
    else if (size == 2) opc = rr(3);
    else opc = rr(4);
    put(0x39000000u | size << 30 | opc << 22 | rr(256) << 10 | 28u << 5 |
        (opc ? rd() : rs()));
}
static void u_ldst_wb(void) {        /* re-base x25 to mid-scratch, then wb */
    put(0x91401399u);                /* ADD x25, x28, #16K: mid-scratch base */
    unsigned size = rr(4), opc = rr(2), mode = rr(2) ? 3 : 1;
    unsigned imm9 = rr(512);         /* signed 9-bit, full range is in-range */
    unsigned t; do t = rd(); while (t == 25);
    put(0x38000000u | size << 30 | opc << 22 | imm9 << 12 | mode << 10 |
        25u << 5 | t);
}
static void u_ldst_ro(void) {
    static const unsigned opt[4] = { 2, 3, 6, 7 };  /* UXTW LSL SXTW SXTX */
    unsigned size = rr(4), opc = rr(2);
    put(0x38200800u | size << 30 | opc << 22 | 27u << 16 | opt[rr(4)] << 13 |
        rr(2) << 12 | 28u << 5 | (opc ? rd() : rs()));
}
static void u_ldstp(void) {
    static const uint32_t mode[3] = { 0xA9000000u, 0xA8800000u, 0xA9800000u };
    unsigned m = rr(3), L = rr(2), t1, t2, base = 28;
    do t1 = rd(); while (t1 == 25);
    do t2 = rd(); while (t2 == t1 || t2 == 25);
    if (m) { put(0x91401399u); base = 25; }        /* wb forms re-base x25 */
    put(mode[m] | L << 22 | rr(32) << 15 | t2 << 10 | base << 5 | t1);
}
static void u_excl(void) {
    put(0x91000399u | (uint32_t)(rr(256) * 8) << 10);   /* ADD x25,x28,#8n */
    unsigned t1, t2, st;
    do t1 = rd(); while (t1 == 25);
    do t2 = rd(); while (t2 == 25);
    do st = rd(); while (st == 25 || st == t2);
    put(0xC85F7C00u | 25u << 5 | t1);                   /* LDXR t1,[x25] */
    put(0xC8007C00u | st << 16 | 25u << 5 | t2);        /* STXR st,t2,[x25] */
}
static void u_ldst_fp(void) {
    static const uint32_t f[6] = { 0xFD400000u, 0xFD000000u, 0xBD400000u,
                                   0xBD000000u, 0x3DC00000u, 0x3D800000u };
    put(f[rr(6)] | rr(256) << 10 | 28u << 5 | rr(32));
}
static void u_fp_2src(void) {
    put(0x1E200800u | rr(2) << 22 | rr(32) << 16 | rr(9) << 12 |
        rr(32) << 5 | rr(32));
}
static void u_fp_1src(void) {
    unsigned type = rr(2), opc = rr(5);              /* FMOV..FSQRT, FCVT */
    if (opc == 4) opc = type ? 4 : 5;                /* FCVT: D->S / S->D */
    put(0x1E204000u | type << 22 | opc << 15 | rr(32) << 5 | rr(32));
}
static void u_fcmp(void) {                           /* FCMP only, not E */
    put(0x1E202000u | rr(2) << 22 | rr(32) << 16 | rr(32) << 5);
}
static void u_fcsel(void) {
    put(0x1E200C00u | rr(2) << 22 | rr(32) << 16 | rr(14) << 12 |
        rr(32) << 5 | rr(32));
}
static void u_fmov_gpr(void) {
    static const uint32_t f[4] = { 0x9E660000u, 0x9E670000u,
                                   0x1E260000u, 0x1E270000u };
    unsigned i = rr(4);
    put(f[i] | rr(32) << 5 | ((i & 1) ? rr(32) : rd()));
}
static void u_cvt(void) {
    static const uint32_t f[6] = { 0x9E620000u, 0x9E630000u, 0x1E220000u,
                                   0x1E230000u, 0x9E780000u, 0x9E790000u };
    unsigned i = rr(6);
    put(f[i] | rr(32) << 5 | (i >= 4 ? rd() : rr(32)));
}
static void u_simd(void) {
    static const uint32_t f[] = {
        0x4E208400u, 0x4E608400u, 0x4EA08400u, 0x4EE08400u,   /* ADD b/h/s/d */
        0x6EA08400u, 0x6EE08400u,                             /* SUB s/d */
        0x4E201C00u, 0x4EA01C00u, 0x6E201C00u, 0x4E601C00u,   /* AND ORR EOR BIC */
        0x4EE01C00u, 0x6E601C00u,                             /* ORN BSL */
        0x6EA08C00u,                                          /* CMEQ.4s */
        0x4E20D400u, 0x4E60D400u, 0x4EA0D400u, 0x4EE0D400u,   /* FADD/FSUB s,d */
        0x6E20DC00u, 0x6E60DC00u,                             /* FMUL s,d */
        0x6E20FC00u, 0x4E20F400u, 0x4EA0F400u                 /* FDIV FMAX FMIN */
    };
    put(f[rr(sizeof f / sizeof f[0])] | rr(32) << 16 | rr(32) << 5 | rr(32));
}
static void u_simd_shift(void) {
    unsigned n = rr(32);
    switch (rr(3)) {
    case 0: put(0x4F005400u | (32 + n) << 16 | rr(32) << 5 | rr(32)); break;
    case 1: put(0x6F000400u | (64 - 1 - n) << 16 | rr(32) << 5 | rr(32)); break;
    default: put(0x4F000400u | (64 - 1 - n) << 16 | rr(32) << 5 | rr(32));
    }
}
static void u_branch(void) {         /* short forward skip over 1-3 units */
    unsigned pos = nw, k = 1 + rr(3);
    put(0);                          /* placeholder, patched below */
    for (unsigned i = 0; i < k; i++) unit(0);
    uint32_t off = nw - pos;         /* words from branch to fall-in point */
    switch (rr(4)) {
    case 0: code[pos] = 0x54000000u | off << 5 | rr(14); break;          /* B.cond */
    case 1: code[pos] = (rr(2) ? 0xB4000000u : 0x34000000u) | off << 5 | rs(); break; /* CBZ */
    case 2: code[pos] = (rr(2) ? 0xB5000000u : 0x35000000u) | off << 5 | rs(); break; /* CBNZ */
    default: {
        unsigned bit = rr(64), r = rs();
        code[pos] = (rr(2) ? 0x36000000u : 0x37000000u) | (bit >> 5) << 31 |
                    (bit & 31) << 19 | off << 5 | r;
    } }
}

static void unit(int allow_branch) {
    unsigned w = rr(allow_branch ? 100 : 92);
    if      (w < 8)  u_addsub_imm();
    else if (w < 14) u_logic_imm();
    else if (w < 19) u_movewide();
    else if (w < 24) u_bitfield();
    else if (w < 26) u_extr();
    else if (w < 33) u_logic_reg();
    else if (w < 40) u_addsub_shifted();
    else if (w < 43) u_addsub_ext();
    else if (w < 46) u_adcsbc();
    else if (w < 50) u_csel();
    else if (w < 53) u_ccmp();
    else if (w < 57) u_dp3();
    else if (w < 60) u_dp2();
    else if (w < 62) u_dp1();
    else if (w < 63) u_adr();
    else if (w < 69) u_ldst_imm();
    else if (w < 71) u_ldst_wb();
    else if (w < 73) u_ldst_ro();
    else if (w < 76) u_ldstp();
    else if (w < 77) u_excl();
    else if (w < 79) u_ldst_fp();
    else if (w < 82) u_fp_2src();
    else if (w < 84) u_fp_1src();
    else if (w < 85) u_fcmp();
    else if (w < 86) u_fcsel();
    else if (w < 87) u_fmov_gpr();
    else if (w < 88) u_cvt();
    else if (w < 91) u_simd();
    else if (w < 92) u_simd_shift();
    else             u_branch();
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s SEED [NINSNS]\n", argv[0]); return 2; }
    uint64_t seed = strtoull(argv[1], NULL, 0);
    rngs = seed * 0x9E3779B97F4A7C15ULL + 1;
    unsigned n = (argc > 2) ? (unsigned)atoi(argv[2]) : 0;
    if (!n) n = 64 + (unsigned)(rnd() % 448);      /* 64..511 units */

    /* Prologue: seed GPRs, reserved bases, V registers. */
    for (unsigned r = 0; r < 26; r++) movzk64(r, rnd());
    movzk64(29, rnd()); movzk64(30, rnd());
    movzk64(26, LOAD_BASE + POOL_OFF);
    movzk64(28, LOAD_BASE + SCRATCH_OFF);
    put(0xD2800000u | (uint32_t)(rnd() & 0x3F8) << 5 | 27);   /* MOVZ x27 */
    for (unsigned v = 0; v < 32; v++)                          /* LDR Qv */
        put(0x3DC00000u | rr(0x100) << 10 | 26u << 5 | v);

    for (unsigned i = 0; i < n; i++) unit(1);

    /* Epilogue: fold V regs + FPSR into x0..x2 (cpu_dump shows only GPRs),
     * then HLT #0. EOR x0, x1, x0, ror #13 mixes to prevent cancellation. */
    for (unsigned v = 0; v < 32; v++) {
        put(0x4E083C00u | v << 5 | 1);             /* UMOV x1, Vv.d[0] */
        put(0xCAC03420u);
        put(0x4E183C00u | v << 5 | 1);             /* UMOV x1, Vv.d[1] */
        put(0xCAC03420u);
    }
    put(0xD53B4420u | 2);                          /* MRS x2, FPSR */
    put(0xD4400000u);                              /* HLT #0 */

    /* Image: code | pad | pool | scratch (both random-initialized). */
    static uint8_t img[BIN_SIZE];
    memcpy(img, code, nw * 4);
    for (unsigned i = POOL_OFF; i < BIN_SIZE; i++) img[i] = (uint8_t)rnd();
    fwrite(img, 1, BIN_SIZE, stdout);
    return 0;
}
