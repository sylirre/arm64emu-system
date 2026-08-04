/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M23 test: engine-parity regressions found by tests/run_fuzz_engines.sh
 * (2026-07-19). Both bugs were x86 -jit only; the test is engine-agnostic
 * and runs under the interpreter, -pd and -jit suites alike.
 *
 * 1. Lazy-NZCV hole: an S-op whose flags are consumed by FCSEL kept host
 *    EFLAGS lazy past the FCSEL, and a following plain ALU op clobbered
 *    them before the next reader (b.cond in a later block) saw them.
 *    Pattern: SUBS ; FCSEL ; ORR ; B.MI.
 * 2. Uncounted NOPs: the JIT frontend consumed PD_NOP without ninsns++,
 *    so NOPs retired in translated code were missing from icount (guest
 *    time skew, -maxinsn overshoot). Checked via CNTVCT_EL0, which this
 *    machine derives from icount, around a block of >128 NOPs (crossing
 *    the block cap so at least one all-NOP block boundary is exercised).
 * 3. IR budget under-reserve: jit_fe_block stopped filling a block at
 *    IR_MAX_OPS - 8, a flat reserve one short of DC ZVA's nine ops, so a
 *    block landing exactly on the boundary appended its terminal IRO_JMP
 *    one past ops[] and the liveness pass then wrote 8 bytes past the
 *    IRBlock allocation. Reproduced by 55 DC ZVA (495 ops) + a pad that
 *    steps the count to exactly 511 + one more DC ZVA.
 *
 * x0=0 on success, else a failing-check id. */

.macro NOP16
    nop; nop; nop; nop; nop; nop; nop; nop
    nop; nop; nop; nop; nop; nop; nop; nop
.endm

/* One IR-budget probe: 55 DC ZVA, `pad` single-op ALU instructions, one
 * more DC ZVA. Entered by a branch over 512-byte alignment padding (never
 * executed): the probe is at most 73 instructions = 292 bytes, so starting
 * on a 512-byte boundary keeps it inside one 4 KB page whatever its offset,
 * and the block cannot end early on jit_fe_block's page-crossing check.
 * That is what makes the op count at the budget boundary exact. The pad
 * sweep then walks ir->n across it one op at a time. */
.macro IRBUDGET pad
    b       991f
    .balign 512
991:
    .rept 55
    dc      zva, x1
    .endr
    .rept \pad
    add     x2, x2, #1
    .endr
    dc      zva, x1
.endm

.text
.global _start
_start:
    /* --- 1. SUBS ; FCSEL ; plain-ALU ; B.cond --- */
    movz    x11, #1
    movz    x1, #1
    fmov    d30, x1
    fmov    d5, xzr
    subs    x5, xzr, x1                 /* 0 - 1: N=1, C=0 */
    fcsel   d3, d30, d5, pl             /* consumes flags (PL: N==0 -> d5) */
    orr     x4, x11, x8, lsl #2         /* plain ALU: EFLAGS clobber on x86 */
    b.mi    1f                          /* must be taken (N=1) */
    b       fail
1:  fmov    x2, d3                      /* PL false -> d5 (= 0) selected */
    cbnz    x2, fail

    /* same shape, flags surviving into a taken CBZ-consumer alternative:
     * ANDS producing Z, FCSEL on NE, EOR clobber, B.EQ */
    movz    x11, #2
    ands    x5, x1, xzr                 /* Z=1 */
    fcsel   d3, d5, d30, ne             /* NE false -> d30 (= 1) */
    eor     x4, x11, x11, lsl #3
    b.eq    2f                          /* must be taken (Z=1) */
    b       fail
2:  fmov    x2, d3
    cmp     x2, #1
    b.ne    fail

    /* --- 2. NOPs must be counted: CNTVCT delta across 144 NOPs --- */
    movz    x11, #3
    mrs     x20, cntvct_el0
    NOP16
    NOP16
    NOP16
    NOP16
    NOP16
    NOP16
    NOP16
    NOP16
    NOP16
    mrs     x21, cntvct_el0
    sub     x22, x21, x20
    cmp     x22, #144                   /* >= 144 insns must have elapsed */
    b.lt    fail

    /* --- 3. IR budget: sweep ir->n across the block-fill boundary --- */
    movz    x11, #4
    movz    x1, #0x4020, lsl #16        /* 64-byte-aligned writable RAM */
    movz    x3, #0xabcd
    str     x3, [x1]                    /* DC ZVA must actually clear this */
    mov     x2, #0
    subs    xzr, x1, x1                 /* Z=1: a flag window over the sweep */
    IRBUDGET 12
    IRBUDGET 13
    IRBUDGET 14
    IRBUDGET 15
    IRBUDGET 16                         /* the one that lands on 511 */
    IRBUDGET 17
    IRBUDGET 18
    IRBUDGET 19
    IRBUDGET 20
    b.ne    fail                        /* neither DC ZVA nor ADD writes NZCV */
    movz    x11, #5
    cmp     x2, #144                    /* 12+13+...+20 */
    b.ne    fail
    movz    x11, #6
    ldr     x3, [x1]
    cbnz    x3, fail                    /* the zeroing itself must be intact */

    movz    x0, #0
    hlt     #0

fail:
    mov     x0, x11
    hlt     #0
