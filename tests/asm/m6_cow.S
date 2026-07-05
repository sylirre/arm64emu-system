/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M6 COW test: emulate the copy-on-write mechanism the kernel relies on.
 * Two VAs (V1,V2) alias one physical page P_shared, mapped read-only. A write
 * to V1 must FAULT; the data-abort handler "breaks COW" by repointing V1's L3
 * entry at a fresh page P_new (RW) + TLBI, then ERETs to re-execute the store.
 * The store must then land in P_new, leaving P_shared (still seen via V2 and via
 * its identity mapping) UNCHANGED. A second case repeats the COW break WITHOUT a
 * TLBI, relying on the spurious-perm-fault re-walk. x0=0 on success, else id.
 *
 * Layout (all phys, identity except the test window):
 *   0x40000000  code (MMU-off entry, then VA-identity)
 *   0x40010000  exception vectors (VBAR_EL1)
 *   0x40080000  stack
 *   0x40090000  L1 table   0x40091000 L2 table   0x40092000 L3 table
 *   test window 0x40400000..0x405fffff via L3 (4 KB pages):
 *     V1   = 0x40400000 (L3[0])   -> P_shared, RO  (handler -> P_new1 RW)
 *     V2   = 0x40401000 (L3[1])   -> P_shared, RO
 *     V3   = 0x40402000 (L3[2])   -> P_shared, RO  (handler -> P_new2 RW, no TLBI)
 *     Pid  = 0x40500000 (L3[0x100]) identity RW   = P_shared
 *     P_new1 = 0x40501000 (reached only via remapped V1)
 *     P_new2 = 0x40502000 (reached only via remapped V3)
 */

.equ V1,        0x40400000
.equ V2,        0x40401000
.equ V3,        0x40402000
.equ PSHARED,   0x40500000
.equ PNEW1,     0x40501000
.equ PNEW2,     0x40502000
.equ L1T,       0x40090000
.equ L2T,       0x40091000
.equ L3T,       0x40092000
.equ AVAL,      0xAAAAAAAA
.equ BVAL,      0xBBBBBBBB
.equ CVAL,      0xCCCCCCCC
/* descriptor attrs: AF(0x400) | valid */
.equ BLK_RW,    0x441          /* 2MB block, AP=01 RW, AF, valid(block=01) */
.equ PG_RW,     0x443          /* 4KB page, AP=01 RW, AF, valid(page=11) */
.equ PG_RO,     0x4C3          /* 4KB page, AP=11 RO, AF, valid(page=11) */

.text
.global _start
_start:
    movz    x0, #0x4008, lsl #16        /* stack 0x40080000 */
    mov     sp, x0
    movz    x0, #0x4001, lsl #16        /* vectors 0x40010000 */
    msr     vbar_el1, x0
    isb

    /* init P_shared = AVAL (MMU off => physical) */
    movz    x0, #PSHARED & 0xffff
    movk    x0, #(PSHARED >> 16), lsl #16
    movz    x1, #AVAL & 0xffff
    movk    x1, #(AVAL >> 16), lsl #16
    str     x1, [x0]

    /* ---- page tables ---- */
    /* L1[1] -> L2T (table desc 0x3) */
    movz    x1, #L1T & 0xffff ; movk x1, #(L1T >> 16), lsl #16
    movz    x2, #L2T & 0xffff ; movk x2, #(L2T >> 16), lsl #16
    orr     x3, x2, #0x3
    str     x3, [x1, #8]                /* L1[1] */

    /* L2[0] = identity 2MB block (covers code/vectors/stack/tables) */
    movz    x4, #0x4000, lsl #16        /* 0x40000000 */
    add     x4, x4, #BLK_RW
    str     x4, [x2]                    /* L2[0] */
    /* L2[2] -> L3T (table desc) covering 0x40400000..0x405fffff */
    movz    x5, #L3T & 0xffff ; movk x5, #(L3T >> 16), lsl #16
    orr     x6, x5, #0x3
    str     x6, [x2, #16]               /* L2[2] (index 2 -> +16 bytes) */

    /* L3 entries: [0]=V1->PSHARED RO, [1]=V2->PSHARED RO, [2]=V3->PSHARED RO,
       [0x100]=identity PSHARED RW */
    movz    x7, #PSHARED & 0xffff ; movk x7, #(PSHARED >> 16), lsl #16
    add     x8, x7, #PG_RO
    str     x8, [x5]                    /* L3[0] */
    str     x8, [x5, #8]                /* L3[1] */
    str     x8, [x5, #16]               /* L3[2] */
    add     x9, x7, #PG_RW
    str     x9, [x5, #0x800]            /* L3[0x100] = 0x100*8 = 0x800 */

    /* MAIR index0 = Normal WB, TCR (T0SZ=25 start L1), enable MMU */
    movz    x4, #0x00ff
    msr     mair_el1, x4
    msr     ttbr0_el1, x1
    movz    x5, #0x3519 ; movk x5, #0x8080, lsl #16 ; movk x5, #0x0002, lsl #32
    msr     tcr_el1, x5
    isb
    mrs     x6, sctlr_el1 ; orr x6, x6, #1 ; msr sctlr_el1, x6 ; isb

    /* ---- check 1: V2 reads the shared AVAL ---- */
    movz    x10, #AVAL & 0xffff ; movk x10, #(AVAL >> 16), lsl #16
    movz    x12, #V2 & 0xffff ; movk x12, #(V2 >> 16), lsl #16
    ldr     w0, [x12]
    movz    x11, #1
    cmp     w0, w10
    b.ne    fail

    /* ---- COW break WITH TLBI ---- */
    mov     x25, #0                     /* fault-seen flag */
    mov     x26, #0                     /* handler mode: 0 = TLBI */
    movz    x13, #V1 & 0xffff ; movk x13, #(V1 >> 16), lsl #16
    movz    x14, #BVAL & 0xffff ; movk x14, #(BVAL >> 16), lsl #16
    str     w14, [x13]                  /* write V1 -> faults -> handler remaps */

    movz    x11, #2                     /* fault must have happened */
    cmp     x25, #1
    b.ne    fail
    movz    x11, #3                     /* V1 now reads BVAL (from P_new1) */
    ldr     w0, [x13]
    cmp     w0, w14
    b.ne    fail
    movz    x11, #4                     /* P_shared via identity UNCHANGED = AVAL */
    movz    x15, #PSHARED & 0xffff ; movk x15, #(PSHARED >> 16), lsl #16
    ldr     w0, [x15]
    cmp     w0, w10
    b.ne    fail                        /* if BVAL here, the write leaked to shared! */
    movz    x11, #5                     /* V2 still reads AVAL */
    ldr     w0, [x12]
    cmp     w0, w10
    b.ne    fail

    /* ---- COW break WITHOUT TLBI (rely on re-walk) ---- */
    mov     x25, #0
    mov     x26, #1                     /* handler mode: 1 = no TLBI */
    movz    x16, #V3 & 0xffff ; movk x16, #(V3 >> 16), lsl #16
    movz    x17, #CVAL & 0xffff ; movk x17, #(CVAL >> 16), lsl #16
    str     w17, [x16]                  /* write V3 -> faults -> handler remaps, no TLBI */

    movz    x11, #6
    cmp     x25, #1
    b.ne    fail
    movz    x11, #7                     /* V3 reads CVAL (re-walk picked up new PTE) */
    ldr     w0, [x16]
    cmp     w0, w17
    b.ne    fail
    movz    x11, #8                     /* P_shared still AVAL */
    ldr     w0, [x15]
    cmp     w0, w10
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

/* ---- data-abort vector: current EL, SP_ELx, sync = VBAR + 0x200 ---- */
    .org    0x10200
    mrs     x20, far_el1                /* faulting VA */
    /* compute L3 entry address for the faulting page:
       idx = (FAR >> 12) & 0x1ff ; entry = L3T + idx*8 */
    lsr     x21, x20, #12
    and     x21, x21, #0x1ff
    movz    x22, #L3T & 0xffff ; movk x22, #(L3T >> 16), lsl #16
    add     x22, x22, x21, lsl #3       /* &L3[idx] */
    /* choose new physical page by mode: mode0 -> PNEW1, mode1 -> PNEW2 */
    cbnz    x26, 1f
    movz    x23, #PNEW1 & 0xffff ; movk x23, #(PNEW1 >> 16), lsl #16
    b       2f
1:  movz    x23, #PNEW2 & 0xffff ; movk x23, #(PNEW2 >> 16), lsl #16
2:  add     x23, x23, #PG_RW
    str     x23, [x22]                  /* repoint the L3 entry to the fresh RW page */
    cbnz    x26, 3f                      /* mode1: skip TLBI (test the re-walk) */
    tlbi    vmalle1
    dsb     ish
    isb
3:  mov     x25, #1                      /* mark fault handled */
    eret
