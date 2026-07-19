/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M25 test: ARM generic timer — CNTPCT/CNTVCT reads, phys-timer IRQ delivery,
 * and WFI wakeup.
 *
 * run_tests.sh runs this as `--bios <img>` (the GICv2 + generic timer exist only
 * when platform_build runs). Executes from flash at 0x0, EL1, MMU off. Because
 * the image is linked at 0x40000000 but loaded at flash 0x0, the vector table
 * base is taken with ADR (PC-relative) — never an absolute movz/literal.
 *
 * The gates pin AE_RTCLOCK=0 (deterministic clock: 1 tick/insn). Under it, WFI
 * fast-forwards the virtual counter to the armed deadline (machine_wait_for_event
 * -> timer_next_deadline_ticks), so the IRQ lands at a fixed icount. Under the
 * real-time clock it instead paces on the host clock; either way the timer fires.
 * x0=0 on success, else a failing-check id. */
.text
.global _start
_start:
    /* vector table (PIC): VBAR must be the runtime address of vbar_base */
    adr     x0, vbar_base
    msr     vbar_el1, x0
    isb

    movz    x8, #0x0800, lsl #16        /* GICD base 0x08000000 */
    movz    x9, #0x0801, lsl #16        /* GICC base 0x08010000 */

    /* enable GIC, unmask priorities, enable + prioritize INTID 30 (phys timer PPI) */
    mov     w0, #1
    str     w0, [x8, #0x000]           /* GICD_CTLR = 1 */
    str     w0, [x9, #0x000]           /* GICC_CTLR = 1 */
    mov     w0, #0xff
    str     w0, [x9, #0x004]           /* GICC_PMR = 0xff */
    mov     w0, #0x80
    strb    w0, [x8, #0x41e]           /* IPRIORITYR[30] = 0x80 */
    movz    w0, #0x4000, lsl #16       /* bit 30 */
    str     w0, [x8, #0x100]           /* ISENABLER word0: enable INTID 30 */

    /* ---- check 1: CNTPCT monotonic ---- */
    mrs     x1, cntpct_el0
    nop
    nop
    mrs     x2, cntpct_el0
    movz    x11, #250
    cmp     x2, x1
    b.ls    fail                        /* must strictly advance */

    /* ---- check 2: CNTVCT tracks CNTPCT (cntvoff == 0) ---- */
    mrs     x1, cntpct_el0
    mrs     x2, cntvct_el0
    sub     x3, x2, x1                   /* virt read one tick after phys */
    movz    x11, #251
    cmp     x2, x1
    b.lo    fail                         /* virt must not lag phys */
    movz    x11, #252
    cmp     x3, #64
    b.hs    fail                         /* and stay within a tiny delta (no offset) */

    /* ---- arm the phys timer to fire in ~1000 ticks, then WFI ---- */
    movz    x0, #1000
    msr     cntp_tval_el0, x0            /* CNTP_CVAL = count + 1000 */
    mov     x0, #1
    msr     cntp_ctl_el0, x0            /* ENABLE, IMASK=0 */
    msr     daifclr, #2                  /* unmask IRQ */
    isb
    movz    x20, #0                      /* sentinel cleared */
    wfi
wfi_ret:
    /* resumed here via ERET from the IRQ handler */
    movz    x11, #253
    movz    x0, #0xa5
    cmp     x20, x0
    b.ne    fail                         /* handler must have run */

    /* IRQ must be unmasked again after ERET (restored from SPSR) */
    mrs     x1, daif
    and     x1, x1, #0x80
    movz    x11, #254
    cbnz    x1, fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

/* ---- exception vectors (current EL, SP_ELx group) ---- */
.org 0x2000
vbar_base:

.org 0x2200                             /* +0x200 sync: unexpected -> report */
    movz    x0, #259
    hlt     #0

.org 0x2280                             /* +0x280 IRQ */
    mrs     x10, cntp_ctl_el0            /* ISTATUS (bit2) must be set */
    and     x12, x10, #4
    cbz     x12, hfail1
    mrs     x13, daif                    /* I (bit7) masked on entry */
    and     x13, x13, #0x80
    cbz     x13, hfail2
    adr     x15, wfi_ret                 /* ELR must point past the WFI */
    mrs     x16, elr_el1
    cmp     x15, x16
    b.ne    hfail3
    movz    x17, #0x0801, lsl #16        /* GICC base */
    ldr     w1, [x17, #0x0c]            /* IAR acks -> 30 (level: pending stays) */
    cmp     w1, #30
    b.ne    hfail4
    msr     cntp_ctl_el0, xzr           /* disable timer: level source deasserts */
    mov     w0, #30
    str     w0, [x17, #0x10]            /* EOIR 30 */
    movz    x20, #0xa5                   /* mark handler ran */
    eret
hfail1: movz x0, #255
    hlt #0
hfail2: movz x0, #256
    hlt #0
hfail3: movz x0, #257
    hlt #0
hfail4: movz x0, #258
    hlt #0
