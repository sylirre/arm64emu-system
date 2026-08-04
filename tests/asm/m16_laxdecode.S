/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M16 test: §4 lax-decode UNDEFs.
 *
 * Unallocated A64 encodings must raise an UNKNOWN synchronous exception
 * (EC 0x00), not silently execute "something". This covers the guards added
 * for TODO_BUGS §4: 3-source multiplies with sf=0, add/sub extended-register
 * shift>4, add/sub shifted-register ROR and 32-bit imm6>=32, logical shifted
 * 32-bit imm6>=32, AdvSIMD copy reserved imm5/imm4/Q (imm5==0, SMOV/UMOV bad
 * size/Q), PMUL with size!=0, the vector LDR Q-form with size!=0, and
 * add/sub-with-carry with bits[15:10]!=0 (the RMIF/SETF neighbourhood).
 *
 * Each unallocated .inst is preceded by a sentinel in x21 (0xffff); the sync
 * handler overwrites x21 with the ESR EC, so after the trap x21 must read 0
 * (EC_UNKNOWN). Interleaved valid neighbours must NOT trap (x21 stays 0xffff),
 * guarding against over-tightening. Runs as a flat -bin image at EL1 (MMU off).
 * x0=0 on success, else a failing-check id. */

.arch armv8.5-a                         /* for the RMIF/SETF/LDAPUR neighbours */

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

    mov     x1, #5                      /* generic arithmetic operands */
    mov     x2, #3
    movz    x5, #0x4020, lsl #16        /* 0x40200000: a readable RAM address */

    /* ================= unallocated: must trap EC_UNKNOWN (x21 -> 0) ========= */

    movz    x21, #0xffff
    .inst   0x1b427c20                  /* SMULH sf=0 */
    movz    x11, #1
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x8b227420                  /* ADD x0,x1,x2,uxtx #5 (shift>4) */
    movz    x11, #2
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x0b028020                  /* ADD w0,w1,w2,lsl #32 (imm6>=32) */
    movz    x11, #3
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x8bc20020                  /* ADD x0,x1,x2,ror (ROR unallocated) */
    movz    x11, #4
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x0a028020                  /* AND w0,w1,w2,lsl #32 (imm6>=32) */
    movz    x11, #5
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x4e000420                  /* AdvSIMD copy imm5==0 (DUP .d) */
    movz    x11, #6
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x0e042c20                  /* SMOV w0,v1.s[0] (size=2,Q=0) */
    movz    x11, #7
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x0e083c20                  /* UMOV w0,v1.d[0] (size=3,Q=0) */
    movz    x11, #8
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x6e629c20                  /* PMUL v0.8h (size!=0) */
    movz    x11, #9
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x7dc00020                  /* LDR (vector) size=1 with Q opc */
    movz    x11, #10
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x1a028020                  /* ADC w0,w1,w2 with bits[15:10]!=0 */
    movz    x11, #11
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xd9800020                  /* LDAPUR-space opc=2 size=3 (no PRFM slot) */
    movz    x11, #12
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x99c00020                  /* LDAPUR-space opc=3 size=2 */
    movz    x11, #13
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x19400420                  /* LDAPUR-space bits[11:10]=01 */
    movz    x11, #14
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x19600020                  /* LDAPUR-space bit21=1 */
    movz    x11, #15
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x3a00042a                  /* RMIF with sf=0 */
    movz    x11, #16
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x1a00082d                  /* SETF8 with S=0 */
    movz    x11, #17
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xba000430                  /* RMIF with o2 (bit4) = 1 */
    movz    x11, #18
    cbnz    x21, fail

    /* SIMD&FP load/store pair with opc==3: unallocated. Decoding it as a
     * pair derived scale=opc+2, i.e. a 32-byte "element" handed to a
     * 16-byte-max vector access — a host buffer overflow, not just a
     * wrong result. Both directions, since only the load smashed the
     * emulator's stack while the store leaked it into guest memory. */
    movz    x21, #0xffff
    .inst   0xed4000a0                  /* LDP V-form opc=3, [x5] */
    movz    x11, #19
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xed0000a0                  /* STP V-form opc=3, [x5] */
    movz    x11, #20
    cbnz    x21, fail

    /* opc==3 is the "load signed, 32-bit result" column, allocated only for
     * the byte and halfword sizes; size==2 (LDRSW is opc==2) and size==3 are
     * both unallocated. Only size==3 used to be rejected. */
    movz    x21, #0xffff
    .inst   0xb9c000a0                  /* size=2 opc=3, unsigned offset */
    movz    x11, #21
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xb8c000a0                  /* size=2 opc=3, unscaled */
    movz    x11, #22
    cbnz    x21, fail

    /* ---- DP-immediate / DP-register fields that must be zero ---- */

    movz    x21, #0xffff
    .inst   0xf3400020                  /* bitfield opc==3 (no instruction) */
    movz    x11, #23
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x93000020                  /* SBFM sf=1 with N=0 */
    movz    x11, #24
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xb3c20020                  /* EXTR with bits[30:29]!=0 */
    movz    x11, #25
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x8b626020                  /* add/sub extended-reg, bits[23:22]!=0 */
    movz    x11, #26
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xbb020c20                  /* MADD with bits[30:29]!=0 */
    movz    x11, #27
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xda420020                  /* CCMP with S=0 */
    movz    x11, #28
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xfa420420                  /* CCMP with bit10=1 */
    movz    x11, #29
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xfa420030                  /* CCMP with o3 (bit4) = 1 */
    movz    x11, #30
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xba820020                  /* CSEL with S=1 */
    movz    x11, #31
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x9a820820                  /* CSEL with bit11=1 */
    movz    x11, #32
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0x52c00000                  /* MOVZ sf=0 with hw=2 */
    movz    x11, #33
    cbnz    x21, fail

    /* The LSE atomic-memory-operation page is defined for V=0 only. The JIT
     * frontend's classifier left bit 26 out of its mask, so under --jit these
     * were translated as real atomics and wrote guest memory. x0 points at
     * RAM so a translated one would be visible, not just wrong. */
    mov     x0, x5
    mov     x1, #7
    movz    x21, #0xffff
    .inst   0xfc210002                  /* LDADD x, V=1 */
    movz    x11, #34
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xbc210002                  /* LDADD w, V=1 */
    movz    x11, #35
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xfc218002                  /* SWP x, V=1 */
    movz    x11, #36
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xfc211002                  /* LDCLR x, V=1 */
    movz    x11, #37
    cbnz    x21, fail

    movz    x21, #0xffff
    .inst   0xfcbfc002                  /* LDAPR x, V=1 */
    movz    x11, #38
    cbnz    x21, fail

    /* ================= valid neighbours: must NOT trap (x21 stays 0xffff) === */

    movz    x21, #0xffff
    smulh   x0, x1, x2                  /* sf=1: valid */
    adcs    x0, x1, x2                  /* add/sub w/ carry bits[15:10]==0 */
    sbc     w0, w1, w2
    rmif    x1, #3, #0xa                /* FLAGM neighbours: valid */
    setf8   w2
    setf16  w2
    add     x0, x1, x2, uxtx #4         /* extended shift==4: valid (max) */
    add     w0, w1, w2, lsl #31         /* shifted 32-bit imm6==31: valid (max) */
    and     w0, w1, w2, lsl #31         /* logical shifted 32-bit imm6==31 */
    orr     x0, x1, x2, ror #4          /* logical ROR still valid */
    dup     v0.2d, v1.d[0]              /* DUP .d needs Q=1: valid */
    umov    x0, v1.d[0]                 /* UMOV Xd,D: valid */
    umov    w0, v1.s[0]                 /* UMOV Wd,S: valid */
    smov    x0, v1.s[0]                 /* SMOV Xd,S: valid */
    smov    w0, v1.h[0]                 /* SMOV Wd,H: valid */
    pmul    v0.16b, v1.16b, v2.16b      /* PMUL .16b: valid */
    ldr     q0, [x5]                    /* vector LDR Q-form (size=0): valid */
    ldr     d0, [x5]                    /* vector LDR D-form: valid */
    ldp     q0, q1, [x5]                /* SIMD&FP pair opc=2: valid (widest) */
    stp     q0, q1, [x5]
    ldp     s0, s1, [x5]                /* SIMD&FP pair opc=0: valid */
    ldrsw   x0, [x5]                    /* size=2 opc=2: valid */
    ldrsh   w0, [x5]                    /* size=1 opc=3: valid (32-bit result) */
    ldrsb   w0, [x5]                    /* size=0 opc=3: valid */
    sbfx    x0, x1, #0, #8              /* bitfield sf=1 N=1: valid */
    ubfx    w0, w1, #0, #8              /* bitfield sf=0 N=0: valid */
    extr    x0, x1, x2, #4              /* EXTR bits[30:29]==0: valid */
    add     x0, x1, x2, uxtx            /* extended-reg bits[23:22]==0: valid */
    madd    x0, x1, x2, x3              /* DP-3-source bits[30:29]==0: valid */
    ccmp    x1, x2, #0, eq              /* CCMP S=1, bit10==0, bit4==0: valid */
    ccmn    x1, #3, #0, ne              /* CCMN immediate form: valid */
    csel    x0, x1, x2, eq              /* CSEL S=0, bit11==0: valid */
    csinv   x0, x1, x2, ne              /* CSINV (op=1, o2=0): valid */
    movz    w0, #0, lsl #16             /* 32-bit move-wide hw==1: valid (max) */
    movz    x0, #0, lsl #32             /* 64-bit move-wide hw==2: valid */
    mov     x0, x5                      /* V=0 LSE neighbours: must all run */
    .inst   0xf8210002                  /* ldadd x1, x2, [x0] */
    .inst   0xb8210002                  /* ldadd w1, w2, [x0] */
    .inst   0xf8218002                  /* swp   x1, x2, [x0] */
    .inst   0xf8211002                  /* ldclr x1, x2, [x0] */
    .inst   0xf8bfc002                  /* ldapr x2, [x0] */
    .inst   0x990000a0                  /* stlur w0,[x5]: valid (LRCPC2) */
    .inst   0xd94000a0                  /* ldapur x0,[x5]: valid (LRCPC2) */
    movz    x11, #100
    movz    x10, #0xffff
    cmp     x21, x10                    /* none of the above may have trapped */
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

    /* vector table base = 0x40010000; sync from current EL/SP_ELx at +0x200.
     * Record the EC, advance ELR past the faulting instruction, and return. */
    .org    0x10200
    mrs     x21, esr_el1
    lsr     x21, x21, #26
    and     x21, x21, #0x3f             /* EC */
    mrs     x22, elr_el1
    add     x22, x22, #4               /* skip the faulting instruction */
    msr     elr_el1, x22
    eret
