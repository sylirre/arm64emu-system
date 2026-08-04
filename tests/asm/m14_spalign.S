/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M14 test: SP-alignment fault (SCTLR_EL1.SA).
 *
 * A misaligned stack pointer used as a load/store base must raise an
 * SP-alignment fault (EC 0x26) when SCTLR_EL1.SA is set, and must NOT fault
 * when SP is 16-byte aligned. Runs as a flat -bin image at EL1 (MMU off).
 * x0=0 on success, else a failing-check id.
 *
 * The atomics get their own section: their translators stage the guest base
 * in an IR temp for the Rt-aliases-Rn hazard, and the JIT's inline check
 * (emit_spchk) keys on the base being VREG_SP, so staging used to hide it. */

.arch armv8.1-a

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

    movz    x8, #0xcafe                 /* store payload */

    /* enable SCTLR_EL1.SA (bit 3) */
    mrs     x6, sctlr_el1
    orr     x6, x6, #8
    msr     sctlr_el1, x6
    isb

    /* --- 1. aligned SP with SA set: must NOT fault --- */
    movz    x21, #0xffff                /* sentinel: overwritten only if a fault is taken */
    str     x8, [sp]                    /* SP is 16-byte aligned -> OK */
    movz    x11, #61
    movz    x10, #0xffff
    cmp     x21, x10                    /* x21 must still be the sentinel */
    b.ne    fail

    /* --- 2. misaligned SP with SA set: must fault with EC=0x26 --- */
    mov     x20, sp                     /* save aligned sp */
    sub     sp, sp, #8                  /* now 8-misaligned (not 16) */
    movz    x21, #0xffff
    str     x8, [sp]                    /* SP-align fault -> handler records EC, skips it */
    mov     sp, x20                     /* restore aligned sp */
    movz    x11, #62
    movz    x10, #0x26                  /* EC_SP_ALIGN */
    cmp     x21, x10
    b.ne    fail

    /* --- 3. every SP-based form must fault when SP is misaligned --- */
    movz    x10, #0x26
    mov     x20, sp
    sub     sp, sp, #8

    .macro SPFAULT id, insn:vararg
    movz    x21, #0xffff
    \insn
    movz    x11, #\id
    cmp     x21, x10
    b.ne    fail2
    .endm

    /* The atomics reach memory through their own translators, which stage
     * the base in a temp for the Rt-aliases-Rn hazard; an SP base must keep
     * its identity through that or the check is skipped. */
    mov     x1, #1
    SPFAULT 79, ldadd x1, x2, [sp]
    SPFAULT 80, swp   x1, x2, [sp]
    SPFAULT 81, cas   x1, x2, [sp]
    SPFAULT 82, ldxr  x2, [sp]
    SPFAULT 83, ldxp  x2, x3, [sp]
    SPFAULT 84, stxr  w4, x2, [sp]

    mov     sp, x20                     /* restore aligned sp */

    /* --- 4. the same forms must all run clean on an aligned SP --- */
    movz    x21, #0xffff
    mov     x1, #1                      /* atomics on an aligned SP: clean */
    ldadd   x1, x2, [sp]
    swp     x1, x2, [sp]
    cas     x1, x2, [sp]
    ldxr    x2, [sp]
    stxr    w4, x2, [sp]
    ldxp    x2, x3, [sp]
    clrex
    movz    x11, #77
    movz    x10, #0xffff
    cmp     x21, x10                    /* none of them may have faulted */
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail2:
    mov     sp, x20                     /* leave SP sane for the HLT path */
    mov     x0, x11
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

    /* vector table base = 0x40010000; sync from current EL/SP_ELx at +0x200.
     * Record the EC, advance ELR past the faulting store, and return. */
    .org    0x10200
    mrs     x21, esr_el1
    lsr     x21, x21, #26
    and     x21, x21, #0x3f             /* EC */
    mrs     x22, elr_el1
    add     x22, x22, #4               /* skip the faulting instruction */
    msr     elr_el1, x22
    eret
