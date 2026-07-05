/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M1 integer interpreter self-test.
 * On success: x0 = 0, HLT.  On failure: x0 = <check id>, HLT.
 * Each CK sets x11 = id; on mismatch we branch to `fail` which moves x11->x0. */

.macro CKX reg, val, id
    movz    x11, #\id
    ldr     x9, =\val
    cmp     \reg, x9
    b.ne    fail
.endm

.macro CKW reg, val, id
    movz    x11, #\id
    ldr     w9, =\val
    cmp     \reg, w9
    b.ne    fail
.endm

.text
.global _start
_start:
    /* ---- move wide ---- */
    movz    x0, #0x1234
    movk    x0, #0xabcd, lsl #16
    CKX     x0, 0xabcd1234, 1
    movn    x1, #0
    CKX     x1, 0xffffffffffffffff, 2
    movn    w2, #0xff
    CKX     x2, 0xffffff00, 3

    /* ---- add/sub immediate ---- */
    movz    x3, #100
    add     x3, x3, #23
    CKX     x3, 123, 4
    sub     x3, x3, #0x1, lsl #12       /* subtract 0x1000 */
    CKX     x3, 0xfffffffffffff07b, 5   /* 123 - 4096 = -3973 */

    /* ---- add/sub shifted register + flags ---- */
    movz    x4, #0x10
    movz    x5, #0x1
    add     x4, x4, x5, lsl #4          /* 0x10 + 0x10 = 0x20 */
    CKX     x4, 0x20, 6
    subs    xzr, x4, #0x20              /* sets Z */
    b.ne    fail2_7
    b       ok7
fail2_7:
    movz    x11, #7
    b       fail
ok7:

    /* ---- logical immediate (must be valid bitmask immediates) ---- */
    movz    x6, #0xffff
    and     x6, x6, #0xff
    CKX     x6, 0xff, 8
    orr     x7, xzr, #0xff00
    CKX     x7, 0xff00, 9
    eor     x7, x7, #0xffff             /* 0xff00 ^ 0xffff = 0x00ff */
    CKX     x7, 0xff, 10

    /* ---- logical shifted register ---- */
    movz    x8, #0xff00
    movz    x9, #0x00ff
    orr     x10, x8, x9                 /* careful: x9 reused by macro; recompute below */
    CKX     x10, 0xffff, 11
    movz    x12, #0xf0f0
    bic     x13, x12, x12, lsr #8       /* 0xf0f0 & ~(0x00f0) = 0xf000 */
    CKX     x13, 0xf000, 12

    /* ---- bitfield aliases ---- */
    movz    x14, #0x0012, lsl #16       /* 0x120000 */
    lsr     x15, x14, #16
    CKX     x15, 0x12, 13
    movz    x16, #0x00ab
    sxtb    x17, w16                    /* sign-extend byte 0xab -> 0xffffffffffffffab */
    CKX     x17, 0xffffffffffffffab, 14
    movz    x18, #0x1234
    ubfx    x19, x18, #4, #8            /* bits [11:4] = 0x23 */
    CKX     x19, 0x23, 15
    movz    x20, #0xffff
    movz    x21, #0
    bfi     x21, x20, #8, #8            /* insert low 8 bits of x20 at pos 8 */
    CKX     x21, 0xff00, 16

    /* ---- extract / ror ---- */
    movz    x22, #0x8001
    ror     x23, x22, #1
    CKX     x23, 0x8000000000004000, 17

    /* ---- conditional select (re-set flags before each; CKX clobbers them) ---- */
    movz    x24, #5
    movz    x25, #9
    cmp     x24, x25
    csel    x26, x24, x25, lt           /* 5<9 -> x24 */
    CKX     x26, 5, 18
    cmp     x24, x25
    cinc    x27, x24, ge                /* ge false -> x24 (no inc) */
    CKX     x27, 5, 19
    cmp     x24, x25
    cset    x28, lt                     /* lt true -> 1 */
    CKX     x28, 1, 20

    /* ---- ccmp ---- */
    movz    x0, #10
    movz    x1, #10
    cmp     x0, x1                      /* Z=1 */
    ccmp    x0, #0, #0, eq              /* eq true -> compare 10,0 -> Z=0 */
    cset    x2, eq
    CKX     x2, 0, 21

    /* ---- mul / madd / smulh / umulh ---- */
    movz    x3, #1000
    movz    x4, #1000
    mul     x5, x3, x4
    CKX     x5, 1000000, 22
    movz    x6, #7
    movz    x7, #6
    movz    x8, #5
    madd    x9, x6, x7, x8              /* 7*6+5 = 47 */
    CKX     x9, 47, 23
    movn    x10, #0                     /* -1 */
    movn    x12, #0                     /* -1 */
    smulh   x13, x10, x12               /* (-1)*(-1) high = 0 */
    CKX     x13, 0, 24
    umulh   x14, x10, x12               /* 0xffff... * 0xffff... high = 0xfffffffffffffffe */
    CKX     x14, 0xfffffffffffffffe, 25
    movz    w15, #0xfffe                /* 32-bit */
    movk    w15, #0xffff, lsl #16       /* w15 = 0xfffffffe */
    smaddl  x16, w15, w15, xzr         /* (-2)*(-2)=4 signed 32->64 */
    CKX     x16, 4, 26

    /* ---- div / variable shifts ---- */
    movz    x17, #100
    movz    x18, #7
    udiv    x19, x17, x18              /* 14 */
    CKX     x19, 14, 27
    movn    x20, #99                   /* -100 */
    sdiv    x21, x20, x18              /* -100/7 = -14 (trunc) */
    CKX     x21, 0xfffffffffffffff2, 28
    movz    x22, #1
    movz    x23, #40
    lslv    x24, x22, x23              /* 1<<40 */
    CKX     x24, 0x10000000000, 29

    /* ---- rev / rbit / clz ---- */
    movz    x25, #0x3412
    movk    x25, #0x7856, lsl #16
    rev32   x26, x25                    /* per-32: 0x12345678 */
    CKX     x26, 0x12345678, 30
    movz    x27, #0
    movz    x27, #0x1, lsl #16          /* 0x10000 */
    clz     x28, x27                    /* 63 - 16 = 47 */
    CKX     x28, 47, 31
    movz    x0, #0x0001
    rbit    x1, x0                      /* bit0 -> bit63 */
    CKX     x1, 0x8000000000000000, 32

    /* ---- loads/stores to RAM scratch at 0x40080000 ---- */
    movz    x2, #0x4008, lsl #16        /* 0x40080000 */
    movz    x3, #0xbeef
    movk    x3, #0xdead, lsl #16
    str     x3, [x2]
    ldr     x4, [x2]
    CKX     x4, 0xdeadbeef, 33
    strb    w3, [x2, #8]
    ldrb    w5, [x2, #8]
    CKX     x5, 0xef, 34
    ldrsb   x6, [x2, #8]               /* 0xef sign-extended */
    CKX     x6, 0xffffffffffffffef, 35
    /* pre/post index */
    movz    x7, #0x4008, lsl #16
    add     x7, x7, #0x100
    movz    x8, #0x55
    str     x8, [x7, #16]!             /* pre: x7 += 16, store */
    CKX     x7, 0x40080110, 36
    ldr     x9, [x7], #8               /* post: load then x7 += 8 */
    CKX     x9, 0x55, 37
    CKX     x7, 0x40080118, 38
    /* stp/ldp */
    movz    x10, #0x4008, lsl #16
    add     x10, x10, #0x200
    movz    x12, #0x11
    movz    x13, #0x22
    stp     x12, x13, [x10]
    ldp     x14, x15, [x10]
    CKX     x14, 0x11, 39
    CKX     x15, 0x22, 40

    /* ---- exclusive ---- */
    movz    x16, #0x4008, lsl #16
    add     x16, x16, #0x300
    ldxr    x17, [x16]
    movz    x18, #0x99
    stxr    w19, x18, [x16]            /* should succeed -> w19=0 */
    CKX     x19, 0, 41
    ldr     x20, [x16]
    CKX     x20, 0x99, 42

    /* ---- branches: cbz/cbnz/tbz/tbnz ---- */
    movz    x21, #0
    cbnz    x21, fail_cb
    cbz     x21, cb_ok
fail_cb:
    movz    x11, #43
    b       fail
cb_ok:
    movz    x22, #0x8
    tbz     x22, #3, fail_tb           /* bit3 is set -> should NOT take */
    b       tb_ok
fail_tb:
    movz    x11, #44
    b       fail
tb_ok:

    /* all good */
    movz    x0, #0
    hlt     #0

fail:
    mov     x0, x11
    hlt     #0
