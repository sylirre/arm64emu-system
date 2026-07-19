/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M31 test: MMU corner cases beyond the m2/m6/m17 happy paths — TBI, EPD, a
 * TxSZ out-of-range translation fault, and a level-0-start (4-level) walk.
 *
 * A flat --bin test (loaded == linked at 0x40000000): builds page tables in RAM
 * with the MMU off, then enables the MMU for each sub-test. The code, tables and
 * scratch all live in [0x40000000, 0x40400000), which every configuration maps
 * identity via an L1 1 GB block, so instruction fetch never faults — only the
 * deliberately out-of-region data access does. Faults are caught at the sync
 * vector (+0x200), which records ESR.DFSC and steps ELR past the faulting load.
 *
 * Sub-tests:
 *   A  TBI0=1: a tagged pointer (top byte set) reads the same location.
 *   B  EPD1=1: any TTBR1 (upper-half) access is a level-0 translation fault,
 *      while the code in TTBR0 keeps running. (EPD0 is not used — it would fault
 *      the code fetch itself, since the code lives in the TTBR0 region.)
 *   C  T0SZ=33 (VA size 31): an access at 2 GB is out of range -> level-0 fault,
 *      chosen so 0x40000000 stays mapped. (T0SZ is clamped to [16,39], so this
 *      stays inside the clamp.)
 *   D  T0SZ=16 (VA size 48): the walk starts at level 0 and descends
 *      L0 -> L1 -> 1 GB block; an access round-trips through it.
 *
 * x0=0 on success, else a failing-check id. */
.text
.global _start
_start:
    /* vectors + MAIR (index 0 = Normal WB), all with the MMU off */
    adr     x0, vbar_base
    msr     vbar_el1, x0
    movz    x0, #0xff
    msr     mair_el1, x0
    isb

    /* ---- build page tables in RAM (MMU off: physical == virtual) ---- */
    movz    x5, #0x4009, lsl #16        /* L1 table @ 0x40090000 */
    ldr     x6, =0x0000000000000441     /* L1[0]: 1 GB block -> PA 0 (AF|AP01|blk) */
    str     x6, [x5]
    ldr     x6, =0x0000000040000441     /* L1[1]: 1 GB block -> PA 0x40000000 */
    str     x6, [x5, #8]
    movz    x7, #0x4009, lsl #16
    movk    x7, #0x1000                 /* L0 table @ 0x40091000 */
    ldr     x6, =0x0000000040090003     /* L0[0]: table desc -> L1 table */
    str     x6, [x7]

    /* ================= A: TBI0 tagged pointer ================= */
    ldr     x0, =0x0000002500000019     /* TCR: T0SZ=25, TBI0=1, IPS=48-bit */
    msr     tcr_el1, x0
    movz    x0, #0x4009, lsl #16        /* TTBR0 = L1 table */
    msr     ttbr0_el1, x0
    isb
    bl      mmu_on
    movz    w1, #0xbeef
    movk    w1, #0xdead, lsl #16        /* 0xDEADBEEF */
    movz    x2, #0x4020, lsl #16        /* untagged VA 0x40200000 */
    str     w1, [x2]
    movz    x3, #0x4020, lsl #16
    movk    x3, #0xff00, lsl #48        /* tagged VA 0xFF00000040200000 */
    ldr     w4, [x3]                     /* TBI: tag ignored -> PA 0x40200000 */
    movz    x11, #370
    cmp     w4, w1
    b.ne    faildis                     /* tag must be ignored, same data */
    bl      mmu_off

    /* ================= B: EPD1 -> TTBR1 walks disabled ================= */
    ldr     x0, =0x0000000500990019     /* TCR: T0SZ=25, T1SZ=25, EPD1=1 */
    msr     tcr_el1, x0
    movz    x0, #0x4009, lsl #16        /* TTBR0 = L1 table (code stays mapped) */
    msr     ttbr0_el1, x0
    isb
    bl      mmu_on
    movz    x28, #0                      /* clear DFSC sentinel */
    movz    x2, #0xffff, lsl #48        /* upper-half VA (bit55=1) -> TTBR1 */
    ldr     w4, [x2]                     /* faults: EPD1 -> level-0 translation */
    movz    x11, #372
    cmp     x28, #0x04                   /* FSC_TRANS_L0 */
    b.ne    faildis
    bl      mmu_off

    /* ================= C: TxSZ out-of-range translation fault ================= */
    ldr     x0, =0x0000000500000021     /* TCR: T0SZ=33 (VA size 31) */
    msr     tcr_el1, x0
    movz    x0, #0x4009, lsl #16        /* TTBR0 = L1 table */
    msr     ttbr0_el1, x0
    isb
    bl      mmu_on
    movz    x28, #0
    movz    x2, #0x8000, lsl #16        /* VA 0x80000000 = 2 GB, past the 31-bit range */
    ldr     w4, [x2]                     /* faults: range check -> level-0 translation */
    movz    x11, #373
    cmp     x28, #0x04
    b.ne    faildis
    bl      mmu_off

    /* ================= D: level-0-start (4-level) walk ================= */
    ldr     x0, =0x0000000500000010     /* TCR: T0SZ=16 (VA size 48) -> start level 0 */
    msr     tcr_el1, x0
    movz    x0, #0x4009, lsl #16
    movk    x0, #0x1000                 /* TTBR0 = L0 table */
    msr     ttbr0_el1, x0
    isb
    bl      mmu_on
    movz    w1, #0xf00d
    movk    w1, #0xcafe, lsl #16        /* 0xCAFEF00D */
    movz    x2, #0x4020, lsl #16        /* VA 0x40200000, mapped via L0->L1->block */
    str     w1, [x2]
    ldr     w4, [x2]
    movz    x11, #374
    cmp     w4, w1
    b.ne    faildis
    bl      mmu_off

    movz    x0, #0
    hlt     #0

/* fault path that first drops back to a known (MMU-off) state for the dump */
faildis:
    mrs     x6, sctlr_el1
    and     x6, x6, #0xfffffffffffffffe
    msr     sctlr_el1, x6
    isb
    mov     x0, x11
    hlt     #0

/* SCTLR_EL1.M toggles (return via LR; handlers/callees touch no stack) */
mmu_on:
    mrs     x6, sctlr_el1
    orr     x6, x6, #1
    msr     sctlr_el1, x6
    isb
    ret
mmu_off:
    mrs     x6, sctlr_el1
    and     x6, x6, #0xfffffffffffffffe
    msr     sctlr_el1, x6
    isb
    ret

.ltorg                                  /* emit the literal pool before the .org gap */

/* ---- exception vectors (current EL, SP_ELx group) ---- */
.org 0x2000
vbar_base:
.org 0x2200                             /* +0x200 synchronous (data abort) */
    mrs     x27, esr_el1
    lsr     x25, x27, #26
    and     x25, x25, #0x3f
    cmp     x25, #0x25                   /* EC_DABORT_SAME */
    b.ne    dabort_bad
    and     x28, x27, #0x3f              /* DFSC = ISS[5:0] */
    mrs     x26, elr_el1
    add     x26, x26, #4                 /* step past the faulting load */
    msr     elr_el1, x26
    eret
dabort_bad:
    movz    x0, #398
    hlt     #0
