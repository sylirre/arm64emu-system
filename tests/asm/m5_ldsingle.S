/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M5 AdvSIMD single-structure load/store self-test: LD1R/LD2R (replicate),
 * single-lane LD1, and single-lane ST1 round-trip. Expected values cross-checked
 * against qemu-system-aarch64. x0=0 on success, else failing-check id. */
.macro CKX reg, val, id
    movz    x11, #\id
    ldr     x9, =\val
    cmp     \reg, x9
    b.ne    fail
.endm

.text
.global _start
_start:
    adr     x1, src

    /* LD1R {v0.16b}, [x1] -> byte src[0]=0x10 replicated to all 16 lanes */
    ld1r    {v0.16b}, [x1]
    umov    x0, v0.d[0]
    CKX     x0, 0x1010101010101010, 1
    umov    x0, v0.d[1]
    CKX     x0, 0x1010101010101010, 2

    /* LD1R {v0.8h} -> halfword src[0:1]=0x1110 replicated */
    ld1r    {v0.8h}, [x1]
    umov    x0, v0.d[0]
    CKX     x0, 0x1110111011101110, 3

    /* LD1R {v0.4s} -> word src[0:3]=0x13121110 replicated */
    ld1r    {v0.4s}, [x1]
    umov    w0, v0.s[2]
    CKX     x0, 0x13121110, 4

    /* LD1R {v0.2d} -> dword src[0:7] replicated */
    ld1r    {v0.2d}, [x1]
    umov    x0, v0.d[1]
    CKX     x0, 0x1716151413121110, 5

    /* LD1R {v0.8b} (64-bit form) -> low 8 lanes set, high half zeroed */
    ld1r    {v0.8b}, [x1]
    umov    x0, v0.d[0]
    CKX     x0, 0x1010101010101010, 6
    umov    x0, v0.d[1]
    CKX     x0, 0x0, 7

    /* LD2R {v0.16b, v1.16b} -> v0=all src[0], v1=all src[1] (de-interleave) */
    ld2r    {v0.16b, v1.16b}, [x1]
    umov    x0, v0.d[1]
    CKX     x0, 0x1010101010101010, 8
    umov    x0, v1.d[0]
    CKX     x0, 0x1111111111111111, 9

    /* single-lane load: seed 0xee, load byte lane 3 = src[0]=0x10, rest kept */
    movi    v0.16b, #0xee
    ld1     {v0.b}[3], [x1]
    umov    x0, v0.d[0]
    CKX     x0, 0xeeeeeeee10eeeeee, 10

    /* single-lane load: seed 0xee, load S lane 1 = 0x13121110, low half kept */
    movi    v0.16b, #0xee
    ld1     {v0.s}[1], [x1]
    umov    x0, v0.d[0]
    CKX     x0, 0x13121110eeeeeeee, 11

    /* single-lane store round-trip: zero scratch, store one 0x10 byte, read back */
    movz    x2, #0x4020, lsl #16        /* RAM scratch 0x40200000 */
    movi    v3.16b, #0
    st1     {v3.16b}, [x2]
    ld1r    {v4.16b}, [x1]              /* v4 = all 0x10 */
    st1     {v4.b}[0], [x2]
    ldr     q5, [x2]
    umov    x0, v5.d[0]
    CKX     x0, 0x0000000000000010, 12

    /* single-lane store, S lane: store 0x13121110 word at scratch+4 */
    movi    v3.16b, #0
    st1     {v3.16b}, [x2]
    ld1r    {v4.4s}, [x1]              /* each lane = 0x13121110 */
    st1     {v4.s}[1], [x2]           /* writes to scratch[0..3] (lane 1 value) */
    ldr     q5, [x2]
    umov    x0, v5.d[0]
    CKX     x0, 0x0000000013121110, 13

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

.align 4
src:
    .byte 0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17
    .byte 0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
