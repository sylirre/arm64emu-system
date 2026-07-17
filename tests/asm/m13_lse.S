/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M13 LSE self-test: ARMv8.1 atomic memory operations (LD<op>/SWP incl. the
 * ST<op> alias and byte/half widths), CAS/CASP (success + fail paths) and
 * LDAPR. x0=0 on success, else failing-check id. Expected values are
 * hand-computed; the same file builds as a Linux user binary with -DUSERMODE
 * (exit code = x0) to cross-check every value against real LSE under
 * qemu-aarch64 before trusting it as the emulator's reference. */
.arch armv8-a+lse

.macro CKX reg, val, id
    movz    x11, #\id
    ldr     x9, =\val
    cmp     \reg, x9
    b.ne    fail
.endm

.text
.global _start
_start:
    /* ---- LDADD x (cross-word carry stays in 64-bit) ---- */
    ldr     x10, =dat64a
    movz    x1, #1
    movk    x1, #1, lsl #32              /* x1 = 0x0000000100000001 */
    ldadd   x1, x2, [x10]
    CKX     x2, 0x1122334455667788, 1
    ldr     x2, [x10]
    CKX     x2, 0x1122334555667789, 2

    /* ---- LDCLR / LDEOR / LDSET w, chained on one word ---- */
    ldr     x12, =dat32a
    movz    w1, #0x0F0F, lsl #16
    movk    w1, #0x00F0
    ldclr   w1, w2, [x12]                /* 0xF0F0F0F0 & ~0x0F0F00F0 */
    CKX     x2, 0xF0F0F0F0, 3
    ldr     w2, [x12]
    CKX     x2, 0xF0F0F000, 4
    movz    w1, #0xFFFF, lsl #16
    ldeor   w1, w2, [x12]
    CKX     x2, 0xF0F0F000, 5
    ldr     w2, [x12]
    CKX     x2, 0x0F0FF000, 6
    movz    w1, #0xFF
    ldset   w1, w2, [x12]
    CKX     x2, 0x0F0FF000, 7
    ldr     w2, [x12]
    CKX     x2, 0x0F0FF0FF, 8

    /* ---- LDSMAX w: signed compare (0x80000001 is negative) ---- */
    ldr     x12, =dat32b
    movz    w1, #1
    ldsmax  w1, w2, [x12]
    CKX     x2, 0x80000001, 9
    ldr     w2, [x12]
    CKX     x2, 1, 10

    /* ---- LDUMAX w: same operands unsigned pick the big one ---- */
    ldr     x12, =dat32c
    movz    w1, #0x8000, lsl #16
    movk    w1, #1
    ldumax  w1, w2, [x12]
    CKX     x2, 1, 11
    ldr     w2, [x12]
    CKX     x2, 0x80000001, 12

    /* ---- LDSMIN / LDUMIN x with a negative operand ---- */
    ldr     x10, =dat64b
    mov     x1, #-3
    ldsmin  x1, x2, [x10]
    CKX     x2, 5, 13
    ldr     x2, [x10]
    CKX     x2, 0xFFFFFFFFFFFFFFFD, 14
    ldr     x10, =dat64c
    ldumin  x1, x2, [x10]                /* unsigned -3 is huge: mem stays 5 */
    CKX     x2, 5, 15
    ldr     x2, [x10]
    CKX     x2, 5, 16

    /* ---- SWP x (with acquire+release ordering bits) ---- */
    ldr     x10, =dat64d
    movz    x1, #0xBBBB
    swpal   x1, x2, [x10]
    CKX     x2, 0xAAAA, 17
    ldr     x2, [x10]
    CKX     x2, 0xBBBB, 18

    /* ---- byte / half widths wrap at their own size ---- */
    ldr     x13, =datb
    movz    w1, #2
    ldaddb  w1, w2, [x13]                /* 0xFF + 2 -> byte 0x01 */
    CKX     x2, 0xFF, 19
    ldrb    w2, [x13]
    CKX     x2, 1, 20
    ldr     x13, =dath
    movz    w1, #3
    ldaddh  w1, w2, [x13]                /* 0xFFFF + 3 -> half 0x0002 */
    CKX     x2, 0xFFFF, 21
    ldrh    w2, [x13]
    CKX     x2, 2, 22

    /* ---- CAS x: success then fail; old value lands in Rs both ways ---- */
    ldr     x10, =dat64e
    movz    x1, #0x1234
    movz    x2, #0x5678
    cas     x1, x2, [x10]
    CKX     x1, 0x1234, 23
    ldr     x2, [x10]
    CKX     x2, 0x5678, 24
    movz    x3, #0x9999
    movz    x4, #0x1111
    cas     x3, x4, [x10]
    CKX     x3, 0x5678, 25
    ldr     x2, [x10]
    CKX     x2, 0x5678, 26

    /* ---- CASAL w ---- */
    ldr     x12, =dat32f
    movz    w5, #0xFFEE
    movk    w5, #0x00C0, lsl #16
    movz    w6, #0xBEEF
    movk    w6, #0xDEAD, lsl #16
    casal   w5, w6, [x12]
    CKX     x5, 0x00C0FFEE, 27
    ldr     w2, [x12]
    CKX     x2, 0xDEADBEEF, 28

    /* ---- ST<op> alias (Rt == XZR) still updates memory ---- */
    ldr     x10, =dat64g
    movz    x1, #23
    stadd   x1, [x10]
    ldr     x2, [x10]
    CKX     x2, 123, 29

    /* ---- CASP x success ---- */
    ldr     x10, =datp
    movz    x4, #0x1111
    movz    x5, #0x2222
    movz    x6, #0x3333
    movz    x7, #0x4444
    casp    x4, x5, x6, x7, [x10]
    CKX     x4, 0x1111, 30
    CKX     x5, 0x2222, 31
    ldp     x2, x3, [x10]
    CKX     x2, 0x3333, 32
    CKX     x3, 0x4444, 33

    /* ---- CASPA w fail: pair loads land in Rs:Rs+1, memory untouched ---- */
    ldr     x12, =datp32
    movz    w4, #7
    movz    w5, #9                       /* second word mismatches (8) */
    movz    w6, #0xAA
    movz    w7, #0xBB
    caspa   w4, w5, w6, w7, [x12]
    CKX     x4, 7, 34
    CKX     x5, 8, 35
    ldr     w2, [x12]
    CKX     x2, 7, 36
    ldr     w2, [x12, #4]
    CKX     x2, 8, 37

    /* ---- LDAPR x (FEAT_LRCPC encoding, .inst to spare the assembler) ---- */
    ldr     x14, =dat64h
    .inst   0xF8BFC1C2                   /* ldapr x2, [x14] */
    CKX     x2, 0xCAFED00D, 38

    /* all good */
    movz    x0, #0
    b       done
fail:
    mov     x0, x11
done:
#ifdef USERMODE
    movz    x8, #93                      /* exit(x0) */
    svc     #0
#else
    hlt     #0
#endif

.data
.balign 16
datp:   .quad 0x1111, 0x2222
dat64a: .quad 0x1122334455667788
dat64b: .quad 5
dat64c: .quad 5
dat64d: .quad 0xAAAA
dat64e: .quad 0x1234
dat64g: .quad 100
dat64h: .quad 0xCAFED00D
datp32: .word 7, 8
dat32a: .word 0xF0F0F0F0
dat32b: .word 0x80000001
dat32c: .word 1
dat32f: .word 0x00C0FFEE
.balign 2
dath:   .hword 0xFFFF
datb:   .byte 0xFF
