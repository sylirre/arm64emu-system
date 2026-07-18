/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M18 test: CPACR_EL1.FPEN trap enforcement (EC 0x07).
 *
 * Out of reset FPEN=0b11 (no trap; architecturally UNKNOWN, chosen so flat
 * images that never program CPACR keep working). Then:
 *  - FPEN=0b00 must trap EVERY FP/SIMD access at EL1: FP data-processing,
 *    FPCR MRS+MSR, and each vector load/store family (LDR q, STR d, LDP q,
 *    LD1 multi-structure, LDR s literal) — while GPR loads stay untrapped;
 *  - FPEN=0b01 must NOT trap EL1 but must trap EL0: the test drops to EL0
 *    via ERET (first EL0 excursion in the suite), the fadd traps to the
 *    lower-EL sync vector (+0x400), the handler enables FPEN=0b11 and
 *    retries, EL0 FP then works, and an SVC returns to the EL1 epilogue.
 * The +0x200 handler records EC (x21) / ESR (x22) and skips the insn.
 * x0=0 on success, else the failing-check id. */

.text
.global _start
_start:
    movz    x0, #0x4018, lsl #16        /* SP_EL1 = 0x40180000 */
    mov     sp, x0
    movz    x0, #0x4001, lsl #16        /* VBAR = 0x40010000 */
    msr     vbar_el1, x0
    isb

    movz    x5, #0x4020, lsl #16        /* 0x40200000: scratch RAM */

    /* ---- 1: FP usable out of reset (FPEN resets to 0b11) ---- */
    movz    x21, #0xffff
    fadd    d0, d0, d0
    movz    x11, #1
    movz    x10, #0xffff
    cmp     x21, x10
    b.ne    fail

    /* ---- FPEN=0b00: everything FP traps at EL1 ---- */
    msr     cpacr_el1, xzr
    isb

    movz    x21, #0xffff                /* 2: FP data-processing */
    fadd    d0, d0, d0
    movz    x11, #2
    cmp     x21, #0x07
    b.ne    fail

    movz    x21, #0xffff                /* 3: MRS FPCR */
    mrs     x3, fpcr
    movz    x11, #3
    cmp     x21, #0x07
    b.ne    fail

    movz    x21, #0xffff                /* 4: MSR FPCR */
    msr     fpcr, xzr
    movz    x11, #4
    cmp     x21, #0x07
    b.ne    fail

    movz    x21, #0xffff                /* 5: vector LDR (register) */
    ldr     q0, [x5]
    movz    x11, #5
    cmp     x21, #0x07
    b.ne    fail

    movz    x21, #0xffff                /* 6: vector STR */
    str     d0, [x5]
    movz    x11, #6
    cmp     x21, #0x07
    b.ne    fail

    movz    x21, #0xffff                /* 7: vector LDP */
    ldp     q0, q1, [x5]
    movz    x11, #7
    cmp     x21, #0x07
    b.ne    fail

    movz    x21, #0xffff                /* 8: LD1 (multiple structures) */
    ld1     {v0.16b}, [x5]
    movz    x11, #8
    cmp     x21, #0x07
    b.ne    fail

    movz    x21, #0xffff                /* 9: vector LDR (literal) */
    ldr     s0, lit4
    movz    x11, #9
    cmp     x21, #0x07
    b.ne    fail

    movz    x21, #0xffff                /* 10: GPR load must NOT trap */
    ldr     x3, [x5]
    movz    x11, #10
    movz    x10, #0xffff
    cmp     x21, x10
    b.ne    fail

    /* ---- FPEN=0b01: EL1 clean, EL0 traps ---- */
    movz    x9, #0x10, lsl #16          /* FPEN=0b01 */
    msr     cpacr_el1, x9
    isb

    movz    x21, #0xffff                /* 11: EL1 FP works under 0b01 */
    fadd    d0, d0, d0
    movz    x11, #11
    movz    x10, #0xffff
    cmp     x21, x10
    b.ne    fail

    movz    x9, #0x4019, lsl #16        /* SP_EL0 = 0x40190000 */
    msr     sp_el0, x9
    adr     x9, el0_code
    msr     elr_el1, x9
    movz    x9, #0x3c0                  /* EL0t, DAIF masked */
    msr     spsr_el1, x9
    eret

el0_code:
    movz    x21, #0xffff                /* 12: EL0 FP traps under 0b01 */
    fadd    d0, d0, d0                  /* -> +0x400; handler sets FPEN=0b11, skips */
    movz    x11, #12
    cmp     x21, #0x07
    b.ne    fail

    movz    x21, #0xffff                /* 13: EL0 FP works after enable */
    fadd    d0, d0, d0
    movz    x11, #13
    movz    x10, #0xffff
    cmp     x21, x10
    b.ne    fail

    svc     #0                          /* -> +0x400 (EC 0x15) -> el1_after */
    movz    x11, #14                    /* not reached */
    b       fail

el1_after:
    movz    x11, #15                    /* 15: arrived via the EL0 SVC */
    cmp     x21, #0x15
    b.ne    fail

    movz    x21, #0xffff                /* 16: EL1 FP sane at the end */
    fadd    d0, d0, d0
    movz    x11, #16
    movz    x10, #0xffff
    cmp     x21, x10
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

lit4:
    .word   0x3f800000                  /* 1.0f for the LDR s literal check */

    /* +0x200: sync from current EL/SP_ELx — record EC/ESR, skip the insn. */
    .org    0x10200
    mrs     x22, esr_el1
    lsr     x21, x22, #26
    and     x21, x21, #0x3f
    mrs     x23, elr_el1
    add     x23, x23, #4
    msr     elr_el1, x23
    eret

    /* +0x400: sync from lower EL (AArch64). SVC ends the EL0 excursion and
     * continues at el1_after (we are at EL1 with SP_EL1 here); an FP trap
     * (EC 0x07) enables FPEN=0b11 and re-enters EL0 past the insn; anything
     * else is a failure. */
    .org    0x10400
    mrs     x22, esr_el1
    lsr     x21, x22, #26
    and     x21, x21, #0x3f
    cmp     x21, #0x15
    b.eq    el1_after
    cmp     x21, #0x07
    b.ne    fail
    movz    x9, #0x30, lsl #16          /* FPEN=0b11 */
    msr     cpacr_el1, x9
    mrs     x23, elr_el1
    add     x23, x23, #4
    msr     elr_el1, x23
    eret
