/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M2 test: enable the MMU (4 KB granule, 1 GB blocks), access memory via VA,
 * and take/return an SVC exception. x0=0 on success, else failing-check id. */

.text
.global _start
_start:
    /* stack (SP_EL1) */
    movz    x0, #0x4030, lsl #16        /* 0x40300000 */
    mov     sp, x0

    /* exception vectors */
    movz    x0, #0x4001, lsl #16        /* 0x40010000 */
    msr     vbar_el1, x0
    isb

    /* L1 table at 0x40090000 with two 1 GB identity blocks (0 and 1 GB) */
    movz    x1, #0x4009, lsl #16        /* table base */
    movz    x2, #0x0441                 /* block: AF(1<<10)|AP=01(1<<6)|valid */
    str     x2, [x1]                    /* L1[0] -> PA 0x00000000 */
    movz    x3, #0x4000, lsl #16        /* 0x40000000 */
    orr     x3, x3, x2
    str     x3, [x1, #8]                /* L1[1] -> PA 0x40000000 */

    msr     ttbr0_el1, x1
    movz    x4, #0x00ff                 /* MAIR index0 = Normal WB */
    msr     mair_el1, x4

    /* TCR_EL1 = 0x0000000280803519 (T0SZ=25 => start at level 1) */
    movz    x5, #0x3519
    movk    x5, #0x8080, lsl #16
    movk    x5, #0x0002, lsl #32
    msr     tcr_el1, x5
    isb

    /* enable MMU */
    mrs     x6, sctlr_el1
    orr     x6, x6, #1
    msr     sctlr_el1, x6
    isb

    /* memory access through translation (identity) */
    movz    x7, #0x4020, lsl #16        /* 0x40200000 */
    movz    x8, #0xcafe
    movk    x8, #0xbabe, lsl #16
    str     x8, [x7]
    ldr     x9, [x7]
    movz    x10, #0xcafe
    movk    x10, #0xbabe, lsl #16
    movz    x11, #50
    cmp     x9, x10
    b.ne    fail

    /* SVC -> handler sets x20=0x1234 then ERET */
    movz    x20, #0
    svc     #0
    movz    x11, #51
    movz    x10, #0x1234
    cmp     x20, x10
    b.ne    fail

    /* also verify ESR_EL1 captured EC=0x15 (SVC64) during the handler */
    movz    x11, #52
    movz    x10, #0x15                  /* handler stored EC into x21 */
    cmp     x21, x10
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

    /* vector table base = 0x40010000; sync from current EL/SP_ELx at +0x200 */
    .org    0x10200
    mrs     x21, esr_el1
    lsr     x21, x21, #26               /* EC field */
    and     x21, x21, #0x3f
    movz    x20, #0x1234
    eret
