/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M4 FP/SIMD self-test: MOVI/MVNI/DUP/INS/UMOV/SMOV + vector load/store.
 * x0=0 on success, else failing-check id. */
.macro CKX reg, val, id
    movz    x11, #\id
    ldr     x9, =\val
    cmp     \reg, x9
    b.ne    fail
.endm

.text
.global _start
_start:
    movz    x20, #0x4020, lsl #16       /* scratch 0x40200000 */

    /* MOVI Vd.16B, #0x12  -> all bytes 0x12 */
    movi    v0.16b, #0x12
    umov    x0, v0.d[0]
    CKX     x0, 0x1212121212121212, 1
    umov    x0, v0.d[1]
    CKX     x0, 0x1212121212121212, 2

    /* MOVI Vd.4S, #0xab, LSL #8 -> each 32-bit lane 0x0000ab00 */
    movi    v1.4s, #0xab, lsl #8
    umov    w0, v1.s[0]
    CKX     x0, 0x0000ab00, 3
    umov    w0, v1.s[3]
    CKX     x0, 0x0000ab00, 4

    /* MVNI Vd.4S, #0x00, LSL #0 -> ~0 = 0xffffffff per lane */
    mvni    v2.4s, #0x00
    umov    w0, v2.s[1]
    CKX     x0, 0xffffffff, 5

    /* DUP general: V3.4S = w-value */
    movz    w5, #0x3456
    movk    w5, #0x12, lsl #16          /* 0x00123456 */
    dup     v3.4s, w5
    umov    w0, v3.s[2]
    CKX     x0, 0x00123456, 6

    /* DUP element: V4.8H = V3.H[0] (0x3456) */
    dup     v4.8h, v3.h[0]
    umov    w0, v4.h[5]
    CKX     x0, 0x3456, 7

    /* INS general then element */
    movz    x6, #0xbeef
    ins     v5.d[1], x6                 /* V5.D[1] = 0xbeef */
    umov    x0, v5.d[1]
    CKX     x0, 0xbeef, 8
    ins     v5.d[0], v5.d[1]            /* copy hi->lo */
    umov    x0, v5.d[0]
    CKX     x0, 0xbeef, 9

    /* SMOV: sign-extend a byte */
    movi    v6.16b, #0x80
    smov    x0, v6.b[0]                 /* 0x80 sign-extended */
    CKX     x0, 0xffffffffffffff80, 10

    /* vector load/store: STR Q, LDR Q round-trip */
    movz    x7, #0xcafe
    movk    x7, #0xdead, lsl #16
    dup     v7.2d, x7                   /* both 64-bit lanes = 0x...deadcafe */
    str     q7, [x20]
    ldr     q8, [x20]
    umov    x0, v8.d[0]
    CKX     x0, 0xdeadcafe, 11
    umov    x0, v8.d[1]
    CKX     x0, 0xdeadcafe, 12

    /* STP/LDP of D registers */
    movi    v9.8b, #0x01
    movi    v10.8b, #0x02
    stp     d9, d10, [x20, #32]
    ldp     d11, d12, [x20, #32]
    umov    x0, v11.d[0]
    CKX     x0, 0x0101010101010101, 13
    umov    x0, v12.d[0]
    CKX     x0, 0x0202020202020202, 14

    /* Compare-against-zero (two-reg misc). v0.8b bytes (lane0..7):
     * 0x00(0) 0x01(+1) 0x7f(+127) 0x80(-128) 0xff(-1) 0x00(0) 0x40(+64) 0xc0(-64)
     * Guards the CMEQ#0 / CMGE#0 distinction (null-byte test in libc strlen). */
    movz    x5, #0x0100
    movk    x5, #0x807f, lsl #16
    movk    x5, #0x00ff, lsl #32
    movk    x5, #0xc040, lsl #48
    fmov    d0, x5
    cmeq    v1.8b, v0.8b, #0            /* ==0   */
    umov    x0, v1.d[0]
    CKX     x0, 0x0000ff00000000ff, 15
    cmgt    v2.8b, v0.8b, #0            /* >0  (signed) */
    umov    x0, v2.d[0]
    CKX     x0, 0x00ff000000ffff00, 16
    cmge    v3.8b, v0.8b, #0            /* >=0 (signed) */
    umov    x0, v3.d[0]
    CKX     x0, 0x00ffff0000ffffff, 17
    cmle    v4.8b, v0.8b, #0            /* <=0 (signed) */
    umov    x0, v4.d[0]
    CKX     x0, 0xff00ffffff0000ff, 18
    cmlt    v5.8b, v0.8b, #0            /* <0  (signed) */
    umov    x0, v5.d[0]
    CKX     x0, 0xff0000ffff000000, 19

    /* FMOV between a GPR and the HIGH 64 bits of a vector (ptype=10, the
     * 128-bit form: `fmov Vd.D[1], Xn` / `fmov Xd, Vn.D[1]`). */
    ldr     x4, =0xaabbccddeeff0011
    fmov    d0, x4                      /* v0.d[0]=x4, v0.d[1]=0 */
    ldr     x3, =0x1122334455667788
    fmov    v0.d[1], x3                 /* hi half <- x3, lo half preserved */
    umov    x0, v0.d[1]
    CKX     x0, 0x1122334455667788, 20
    umov    x0, v0.d[0]
    CKX     x0, 0xaabbccddeeff0011, 21  /* low half untouched by the hi write */
    fmov    x5, v0.d[1]                 /* hi half -> GPR (opcode=6 direction) */
    mov     x0, x5
    CKX     x0, 0x1122334455667788, 22

    /* AdvSIMD scalar two-register-misc int<->FP convert (operand in a vector
     * lane, e.g. `ucvtf d,d`). U=unsigned, sz=0 single / 1 double. */
    movz    x4, #0x4240                 /* 1000000 = 0xf4240 */
    movk    x4, #0xf, lsl #16
    fmov    d0, x4                      /* integer bits into v0.d[0] */
    ucvtf   d0, d0                      /* d0 = 1000000.0 (double) */
    fcvtzu  x0, d0                      /* GPR-form back-convert as oracle */
    CKX     x0, 1000000, 23
    fcvtzu  d1, d0                      /* scalar-misc fp->uint, result in lane */
    umov    x0, v1.d[0]
    CKX     x0, 1000000, 24

    mov     x4, #-5
    fmov    d2, x4                      /* -5 (two's complement) into the lane */
    scvtf   d2, d2                      /* signed int -> -5.0 */
    fcvtzs  d3, d2                      /* round-toward-zero back to integer */
    umov    x0, v3.d[0]
    CKX     x0, 0xfffffffffffffffb, 25  /* -5 */

    movz    w4, #0xabcd                 /* single-precision path (sz=0) */
    fmov    s4, w4
    ucvtf   s4, s4                      /* 0xabcd -> 43981.0f */
    fcvtzu  w0, s4
    CKX     x0, 0xabcd, 26

    /* AdvSIMD scalar shift-by-immediate (D-form): SHL/USHR/SSHR, e.g. the
     * `shl d,d,#52` libc float formatting uses to assemble a mantissa. */
    movz    x4, #0x7
    fmov    d0, x4
    shl     d0, d0, #52                 /* 7 << 52 */
    umov    x0, v0.d[0]
    CKX     x0, 0x0070000000000000, 27
    ldr     x4, =0xff00000000000000
    fmov    d1, x4
    ushr    d2, d1, #4                  /* logical >> 4 */
    umov    x0, v2.d[0]
    CKX     x0, 0x0ff0000000000000, 28
    sshr    d3, d1, #4                  /* arithmetic >> 4 (sign bit set) */
    umov    x0, v3.d[0]
    CKX     x0, 0xfff0000000000000, 29

    /* Scalar fixed-point convert (bit21=0, #fbits scale): SCVTF/UCVTF int->fp
     * divides by 2^fbits; FCVTZS/FCVTZU fp->int multiplies then truncates. */
    movz    w4, #10
    scvtf   d0, w4, #2                  /* 10 / 2^2 = 2.5 */
    fmov    x0, d0
    CKX     x0, 0x4004000000000000, 30  /* 2.5 */
    fcvtzs  w0, d0, #2                  /* trunc(2.5 * 4) = 10 */
    CKX     x0, 10, 31
    movz    w4, #6
    ucvtf   s1, w4, #1                  /* 6 / 2 = 3.0 (single) */
    fmov    w0, s1
    CKX     x0, 0x40400000, 32          /* 3.0f */
    fcvtzu  w0, s1, #1                  /* trunc(3.0 * 2) = 6 */
    CKX     x0, 6, 33

    /* FCVTZS/FCVTZU of a NaN must yield 0 (FPToFixed), matching hardware, qemu
     * and the interpreter — the x86 JIT NaN-gates before cvttsd2si, which would
     * otherwise return the 0x80000000... integer-indefinite. */
    ldr     x4, =0x7ff8000000000000     /* quiet NaN (double) */
    fmov    d0, x4
    fcvtzs  x0, d0
    CKX     x0, 0, 34
    fcvtzu  x0, d0
    CKX     x0, 0, 35
    fcvtzs  w0, d0
    CKX     x0, 0, 36
    fcvtzu  w0, d0
    CKX     x0, 0, 37
    movz    w4, #0x7fc0, lsl #16        /* single quiet NaN 0x7fc00000 */
    fmov    s1, w4
    fcvtzs  x0, s1
    CKX     x0, 0, 38
    fcvtzu  w0, s1
    CKX     x0, 0, 39

    /* AArch64 NaN results (exec_fpsimd.c fpnan_*). An invalid operation whose
     * operands hold no NaN gives the architecture's DefaultNaN, which is
     * POSITIVE; the x86 host answers with the negative "real indefinite" QNaN
     * and the interpreter used to inherit that sign. FNMUL is the one form
     * whose generated NaN really is negative -- it applies FPNeg after FPMul,
     * and FPNeg flips a NaN's sign too. */
    fmov    d0, xzr                     /* +0.0 */
    fdiv    d1, d0, d0                  /* 0/0 -> +DefaultNaN */
    fmov    x0, d1
    CKX     x0, 0x7ff8000000000000, 40
    ldr     x4, =0x7ff0000000000000     /* +inf */
    fmov    d3, x4
    fnmul   d1, d0, d3                  /* 0*inf invalid, then FPNeg */
    fmov    x0, d1
    CKX     x0, 0xfff8000000000000, 41
    fmov    d2, #-1.0
    fsqrt   d1, d2                      /* sqrt(-1) -> +DefaultNaN */
    fmov    x0, d1
    CKX     x0, 0x7ff8000000000000, 42
    fmov    s0, wzr
    fdiv    s1, s0, s0
    fmov    w0, s1
    CKX     x0, 0x7fc00000, 43
    movi    v0.4s, #0
    fdiv    v1.4s, v0.4s, v0.4s         /* vector, every lane */
    umov    x0, v1.d[0]
    CKX     x0, 0x7fc000007fc00000, 44

    /* Propagation is decided here too, so it no longer depends on which
     * operand the host compiler made the destination: a signalling operand is
     * taken first and quieted, otherwise the first quiet one in the
     * architecture's order -- and for the fused multiply-add family that order
     * starts at the ADDEND. */
    movz    w4, #0x7fc0, lsl #16
    movk    w4, #0xaaaa                 /* +qNaN payload aaaa */
    fmov    s2, w4
    movz    w4, #0x7fc0, lsl #16
    movk    w4, #0xbbbb                 /* +qNaN payload bbbb */
    fmov    s3, w4
    fadd    s1, s2, s3                  /* first operand wins */
    fmov    w0, s1
    CKX     x0, 0x7fc0aaaa, 45
    movz    w4, #0x7f80, lsl #16
    movk    w4, #0xbbbb                 /* sNaN payload bbbb */
    fmov    s3, w4
    fadd    s1, s2, s3                  /* signalling beats a quiet first */
    fmov    w0, s1
    CKX     x0, 0x7fc0bbbb, 46
    movz    w4, #0x7fc0, lsl #16
    movk    w4, #0xcccc
    fmov    s4, w4                      /* addend +qNaN payload cccc */
    movz    w4, #0x7fc0, lsl #16
    movk    w4, #0xbbbb
    fmov    s3, w4
    fmadd   s1, s2, s3, s4              /* addend is scanned first */
    fmov    w0, s1
    CKX     x0, 0x7fc0cccc, 47
    fmov    s5, #1.0
    fmsub   s1, s2, s5, s5              /* FPNeg(n) reaches the NaN's sign */
    fmov    w0, s1
    CKX     x0, 0xffc0aaaa, 48

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0
