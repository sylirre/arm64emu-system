/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M27 test: load/store-exclusive monitor (LDXR/STXR/LDXP/STXP + CLREX).
 *
 * A flat --bin test (loaded == linked at 0x40000000): runs at EL1 with the MMU
 * off, scratch RAM at 0x40200000. Covers the portable monitor semantics that
 * agree with real hardware / QEMU:
 *   - clean LDXR/STXR success and re-arm,
 *   - CLREX clears the monitor,
 *   - STXR with no prior LDXR fails,
 *   - address-mismatch STXR fails,
 *   - the monitor is cleared by taking an exception (the headline system-mode
 *     property: an IRQ/SVC between a kernel LDXR and its STXR must make the STXR
 *     fail, or a concurrent update is silently dropped),
 *   - byte/half/word/doubleword widths and the LDXP/STXP pair.
 *
 * Deliberately NOT asserted (this model diverges from real HW / QEMU, both
 * CONSTRAINED UNPREDICTABLE): an intervening plain STR does not clear the
 * monitor here, and STXR ignores excl_size so a size-mismatched STXR at the same
 * address succeeds. x0=0 on success, else a failing-check id. */
.text
.global _start
_start:
    adr     x9, vbar_base
    msr     vbar_el1, x9
    isb
    movz    x0, #0x4020, lsl #16        /* scratch A = 0x40200000 */
    add     x5, x0, #0x40               /* scratch B (different address) */

    /* ---- check 1: clean word LDXR/STXR success ---- */
    movz    w3, #0x1234
    str     w3, [x0]
    ldxr    w1, [x0]
    movz    w6, #0x5678
    stxr    w2, w6, [x0]
    movz    x11, #270
    cbnz    w2, fail                    /* w2==0 => success */
    ldr     w4, [x0]
    movz    x11, #271
    cmp     w4, w6
    b.ne    fail                        /* memory updated */

    /* ---- check 2: CLREX clears the monitor ---- */
    movz    w3, #0x1111
    str     w3, [x0]
    ldxr    w1, [x0]
    clrex
    movz    w6, #0x2222
    stxr    w2, w6, [x0]
    movz    x11, #272
    cmp     w2, #1
    b.ne    fail                        /* must fail */
    ldr     w4, [x0]
    movz    x11, #273
    cmp     w4, w3
    b.ne    fail                        /* memory unchanged */

    /* ---- check 3: STXR with no prior LDXR fails ---- */
    clrex
    movz    w3, #0x3333
    str     w3, [x0]
    movz    w6, #0x4444
    stxr    w2, w6, [x0]
    movz    x11, #274
    cmp     w2, #1
    b.ne    fail
    ldr     w4, [x0]
    movz    x11, #275
    cmp     w4, w3
    b.ne    fail

    /* ---- check 4: address-mismatch STXR fails ---- */
    movz    w3, #0x5a5a
    str     w3, [x0]
    str     w3, [x5]
    ldxr    w1, [x0]                     /* arm address A */
    movz    w6, #0x6b6b
    stxr    w2, w6, [x5]                 /* store to B != A */
    movz    x11, #276
    cmp     w2, #1
    b.ne    fail
    ldr     w4, [x5]
    movz    x11, #277
    cmp     w4, w3
    b.ne    fail                        /* B unchanged */

    /* ---- check 5: exception clears the monitor (headline) ---- */
    clrex
    movz    w3, #0x0f0f
    str     w3, [x0]
    ldxr    w1, [x0]                     /* arm */
    svc     #0                           /* exception_take clears excl_valid */
    movz    w6, #0x7070
    stxr    w2, w6, [x0]                 /* must fail */
    movz    x11, #278
    cmp     w2, #1
    b.ne    fail
    ldr     w4, [x0]
    movz    x11, #279
    cmp     w4, w3
    b.ne    fail                        /* unchanged */

    /* ---- check 6: byte / half / doubleword widths succeed ---- */
    movz    w3, #0x7e
    strb    w3, [x0]
    ldxrb   w1, [x0]
    movz    w6, #0xa5
    stxrb   w2, w6, [x0]
    movz    x11, #280
    cbnz    w2, fail
    ldrb    w4, [x0]
    movz    x11, #281
    cmp     w4, w6
    b.ne    fail

    movz    w3, #0x1234
    strh    w3, [x0]
    ldxrh   w1, [x0]
    movz    w6, #0xbeef
    stxrh   w2, w6, [x0]
    movz    x11, #282
    cbnz    w2, fail
    ldrh    w4, [x0]
    movz    x11, #283
    cmp     w4, w6
    b.ne    fail

    ldr     x3, =0x1122334455667788
    str     x3, [x0]
    ldxr    x1, [x0]
    ldr     x6, =0x99aabbccddeeff00
    stxr    w2, x6, [x0]
    movz    x11, #284
    cbnz    w2, fail
    ldr     x4, [x0]
    movz    x11, #285
    cmp     x4, x6
    b.ne    fail

    /* ---- check 7: LDXP / STXP pair ---- */
    ldr     x3, =0xcafef00dcafef00d
    str     x3, [x0]
    str     x3, [x0, #8]
    ldxp    x1, x7, [x0]
    ldr     x6, =0x0123456789abcdef
    ldr     x8, =0xfedcba9876543210
    stxp    w2, x6, x8, [x0]
    movz    x11, #286
    cbnz    w2, fail
    ldr     x4, [x0]
    movz    x11, #287
    cmp     x4, x6
    b.ne    fail
    ldr     x4, [x0, #8]
    movz    x11, #288
    cmp     x4, x8
    b.ne    fail

    /* ---- check 8: monitor re-arms after a successful STXR ---- */
    movz    w3, #0x0001
    str     w3, [x0]
    ldxr    w1, [x0]
    movz    w6, #0x0002
    stxr    w2, w6, [x0]
    movz    x11, #289
    cbnz    w2, fail
    ldxr    w1, [x0]                     /* re-arm */
    movz    w6, #0x0003
    stxr    w2, w6, [x0]
    movz    x11, #290
    cbnz    w2, fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

.ltorg                                  /* emit the literal pool before the .org gap */

/* ---- exception vectors (current EL, SP_ELx group) ---- */
.org 0x2000
vbar_base:
.org 0x2200                             /* +0x200 synchronous (SVC) */
    mrs     x28, esr_el1
    lsr     x28, x28, #26
    cmp     x28, #0x15                   /* EC_SVC64: expected */
    b.ne    sync_bad
    eret
sync_bad:
    movz    x0, #299
    hlt     #0
