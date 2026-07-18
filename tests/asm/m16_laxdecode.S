/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M16 test: §4 lax-decode UNDEFs.
 *
 * Unallocated A64 encodings must raise an UNKNOWN synchronous exception
 * (EC 0x00), not silently execute "something". This covers the guards added
 * for TODO_BUGS §4: 3-source multiplies with sf=0, add/sub extended-register
 * shift>4, add/sub shifted-register ROR and 32-bit imm6>=32, logical shifted
 * 32-bit imm6>=32, AdvSIMD copy reserved imm5/imm4/Q (imm5==0, SMOV/UMOV bad
 * size/Q), PMUL with size!=0, and the vector LDR Q-form with size!=0.
 *
 * Each unallocated .inst is preceded by a sentinel in x21 (0xffff); the sync
 * handler overwrites x21 with the ESR EC, so after the trap x21 must read 0
 * (EC_UNKNOWN). Interleaved valid neighbours must NOT trap (x21 stays 0xffff),
 * guarding against over-tightening. Runs as a flat -bin image at EL1 (MMU off).
 * x0=0 on success, else a failing-check id. */

.text
.global _start
_start:
    /* stack (SP_EL1), 16-byte aligned */
    movz    x0, #0x4030, lsl #16        /* 0x40300000 */
    mov     sp, x0

    /* exception vectors at 0x40010000 */
    movz    x0, #0x4001, lsl #16
    msr     vbar_el1, x0
    isb

    mov     x1, #5                      /* generic arithmetic operands */
    mov     x2, #3
    movz    x5, #0x4020, lsl #16        /* 0x40200000: a readable RAM address */

    /* ================= unallocated: must trap EC_UNKNOWN (x21 -> 0) ========= */

    movz    x21, #0xffff
    .inst   0x1b427c20                  /* SMULH sf=0 */
    movz    x11, #1
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x8b227420                  /* ADD x0,x1,x2,uxtx #5 (shift>4) */
    movz    x11, #2
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x0b028020                  /* ADD w0,w1,w2,lsl #32 (imm6>=32) */
    movz    x11, #3
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x8bc20020                  /* ADD x0,x1,x2,ror (ROR unallocated) */
    movz    x11, #4
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x0a028020                  /* AND w0,w1,w2,lsl #32 (imm6>=32) */
    movz    x11, #5
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x4e000420                  /* AdvSIMD copy imm5==0 (DUP .d) */
    movz    x11, #6
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x0e042c20                  /* SMOV w0,v1.s[0] (size=2,Q=0) */
    movz    x11, #7
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x0e083c20                  /* UMOV w0,v1.d[0] (size=3,Q=0) */
    movz    x11, #8
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x6e629c20                  /* PMUL v0.8h (size!=0) */
    movz    x11, #9
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x7dc00020                  /* LDR (vector) size=1 with Q opc */
    movz    x11, #10
    cbnz    x21, fail

    /* ================= valid neighbours: must NOT trap (x21 stays 0xffff) === */

    movz    x21, #0xffff
    smulh   x0, x1, x2                  /* sf=1: valid */
    add     x0, x1, x2, uxtx #4         /* extended shift==4: valid (max) */
    add     w0, w1, w2, lsl #31         /* shifted 32-bit imm6==31: valid (max) */
    and     w0, w1, w2, lsl #31         /* logical shifted 32-bit imm6==31 */
    orr     x0, x1, x2, ror #4          /* logical ROR still valid */
    dup     v0.2d, v1.d[0]              /* DUP .d needs Q=1: valid */
    umov    x0, v1.d[0]                 /* UMOV Xd,D: valid */
    umov    w0, v1.s[0]                 /* UMOV Wd,S: valid */
    smov    x0, v1.s[0]                 /* SMOV Xd,S: valid */
    smov    w0, v1.h[0]                 /* SMOV Wd,H: valid */
    pmul    v0.16b, v1.16b, v2.16b      /* PMUL .16b: valid */
    ldr     q0, [x5]                    /* vector LDR Q-form (size=0): valid */
    ldr     d0, [x5]                    /* vector LDR D-form: valid */
    movz    x11, #100
    movz    x10, #0xffff
    cmp     x21, x10                    /* none of the above may have trapped */
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

    /* vector table base = 0x40010000; sync from current EL/SP_ELx at +0x200.
     * Record the EC, advance ELR past the faulting instruction, and return. */
    .org    0x10200
    mrs     x21, esr_el1
    lsr     x21, x21, #26
    and     x21, x21, #0x3f             /* EC */
    mrs     x22, elr_el1
    add     x22, x22, #4               /* skip the faulting instruction */
    msr     elr_el1, x22
    eret
