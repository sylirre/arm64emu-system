/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M26 test: IRQ + synchronous exception entry — vector offsets, DAIF masking on
 * entry, banked SPSR/ELR/ESR, and ERET.
 *
 * run_tests.sh runs this as `--bios <img>` (the GICv2 exists only under
 * platform_build). Executes from flash at 0x0, EL1, MMU off; VBAR is taken with
 * ADR (PIC) because the image is linked at 0x40000000 but loaded at flash 0x0.
 *
 * A spare SPI (INTID 32) is injected in software via ISPENDR (GICD_SGIR is a
 * no-op in this single-CPU model) and configured edge-triggered so IAR
 * auto-clears it. The CPU spins on a one-instruction self-branch so the IRQ is
 * always taken at that exact boundary on every engine, letting the handler
 * assert ELR precisely. An SVC #7 then cross-checks the synchronous vector at
 * +0x200 vs the IRQ vector at +0x280. FIQ is not exercised: nothing in this
 * model drives c->fiq_line (the GIC only asserts irq_line). x0=0 on success,
 * else a failing-check id. */
.text
.global _start
_start:
    adr     x0, vbar_base
    msr     vbar_el1, x0
    isb

    movz    x8, #0x0800, lsl #16        /* GICD base 0x08000000 */
    movz    x9, #0x0801, lsl #16        /* GICC base 0x08010000 */
    mov     w0, #1
    str     w0, [x8, #0x000]           /* GICD_CTLR = 1 */
    str     w0, [x9, #0x000]           /* GICC_CTLR = 1 */
    mov     w0, #0xff
    str     w0, [x9, #0x004]           /* GICC_PMR = 0xff */
    mov     w0, #0x80
    strb    w0, [x8, #0x420]           /* IPRIORITYR[32] = 0x80 */
    mov     w0, #0x2
    str     w0, [x8, #0xc08]           /* ICFGR: INTID 32 edge */
    mov     w0, #0x1
    str     w0, [x8, #0x104]           /* ISENABLER word1: enable INTID 32 */

    movz    x20, #0                     /* handler sentinel */
    movz    x21, #0                     /* SVC sentinel */

    /* deliver the IRQ, then wait for the handler on a self-branch */
    msr     daifclr, #2                 /* unmask IRQ */
    isb
    mov     w0, #1
    str     w0, [x8, #0x204]           /* ISPENDR: pend INTID 32 -> irq_line=1 */
spin:
    cbz     x20, spin                   /* IRQ is always taken at this boundary */

    /* ---- back from the IRQ handler ---- */
    movz    x11, #260
    movz    x0, #0x5a
    cmp     x20, x0
    b.ne    fail                        /* handler must have run */
    mrs     x1, daif                    /* I unmasked again after ERET */
    and     x1, x1, #0x80
    movz    x11, #261
    cbnz    x1, fail

    /* ---- synchronous cross-check: SVC #7 -> +0x200 ---- */
    svc     #7
    movz    x11, #262
    cmp     x21, #1
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

/* ---- exception vectors (current EL, SP_ELx group) ---- */
.org 0x2000
vbar_base:

.org 0x2200                             /* +0x200 synchronous */
    mrs     x1, esr_el1
    lsr     x2, x1, #26
    and     x2, x2, #0x3f               /* EC */
    cmp     x2, #0x15                   /* EC_SVC64 */
    b.ne    shfail1
    and     x3, x1, #0xffff             /* ISS = imm16 */
    cmp     x3, #7
    b.ne    shfail2
    movz    x21, #1
    eret
shfail1: movz x0, #269
    hlt #0
shfail2: movz x0, #270
    hlt #0

.org 0x2280                             /* +0x280 IRQ */
    mrs     x1, daif                    /* all of DAIF masked on entry ([9:6]) */
    and     x1, x1, #0x3c0
    movz    x14, #0x3c0
    cmp     x1, x14
    b.ne    ihfail1
    mrs     x2, spsr_el1                /* captured pre-entry PSTATE */
    and     x3, x2, #0x80
    cbnz    x3, ihfail2                 /* SPSR.I must be 0 (was unmasked) */
    and     x4, x2, #0xf
    cmp     x4, #5                       /* M = EL1h */
    b.ne    ihfail3
    mrs     x5, esr_el1
    cbnz    x5, ihfail4                  /* IRQ delivered with esr=0 */
    adr     x6, spin
    mrs     x7, elr_el1
    cmp     x6, x7
    b.ne    ihfail5                      /* ELR points at the interrupted spin */
    movz    x17, #0x0801, lsl #16        /* GICC base */
    ldr     w1, [x17, #0x0c]            /* IAR -> 32 (edge auto-clears pending) */
    cmp     w1, #32
    b.ne    ihfail6
    mov     w0, #32
    str     w0, [x17, #0x10]            /* EOIR 32 */
    movz    x20, #0x5a                   /* mark handler ran */
    eret
ihfail1: movz x0, #263
    hlt #0
ihfail2: movz x0, #264
    hlt #0
ihfail3: movz x0, #265
    hlt #0
ihfail4: movz x0, #266
    hlt #0
ihfail5: movz x0, #267
    hlt #0
ihfail6: movz x0, #268
    hlt #0
