/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M24 test: GICv2 distributor + CPU-interface state machine.
 *
 * run_tests.sh runs this one as `--bios <img>`, not `--bin`: the GICv2 (like
 * every MMIO device) is created only when platform_build runs under
 * --bios/--kernel. It executes from flash at 0x0, at EL1 with the MMU off, so
 * MMIO addresses are the physical device windows (GICD 0x08000000, GICC
 * 0x08010000).
 *
 * The whole test keeps DAIF.I masked and drives the GIC purely through MMIO:
 * gic_update runs synchronously inside every distributor/CPU-interface write, so
 * HPPIR/IAR/EOIR reflect the new state immediately with no spinning — fully
 * deterministic across all three engines. Two spare SPIs (INTID 32 and 33) are
 * configured edge-triggered so IAR auto-clears pending, giving a clean
 * priority-ordering sequence. x0=0 on success, else a failing-check id.
 *
 * Cross-checked against qemu-system-aarch64 -M virt,gic-version=2 -cpu
 * cortex-a57: checks 1-5 and 7 (priority ordering, the PMR strict-less-than
 * boundary, ISACTIVER/EOIR, ICPENDR, ICENABLER and GICD_CTLR gating) match real
 * QEMU. Check 6 (ITARGETSR for a PPI/SGI reads 0x01) is the one intentional
 * divergence: GICv2 (IHI 0048B) makes ITARGETSR0-7 read-only, returning the
 * reading CPU's own targets mask (0x01 for CPU0), and Linux gic_get_cpumask()
 * relies on it — so the emulator returns 0x01 (see gicv2.c), whereas this QEMU
 * build reports 0x00 for the single-CPU virt machine.
 */
.text
.global _start
_start:
    movz    x8, #0x0800, lsl #16        /* GICD base 0x08000000 */
    movz    x9, #0x0801, lsl #16        /* GICC base 0x08010000 */

    /* enable distributor + CPU interface, unmask all priorities */
    mov     w0, #1
    str     w0, [x8, #0x000]            /* GICD_CTLR = 1 */
    str     w0, [x9, #0x000]            /* GICC_CTLR = 1 */
    mov     w0, #0xff
    str     w0, [x9, #0x004]            /* GICC_PMR  = 0xff (unmask) */

    /* INTID 32,33 edge-triggered (ICFGR word for 16..31 is at 0xc08; field
     * (intid%16)*2, edge = high bit): 32 -> bits[1:0]=0b10, 33 -> bits[3:2]=0b10 */
    mov     w0, #0xa
    str     w0, [x8, #0xc08]           /* ICFGR: 32,33 edge */
    mov     w0, #0x3
    str     w0, [x8, #0x104]           /* ISENABLER word1: enable 32,33 */

    /* ---- check 1: priority ordering (33 higher priority than 32) ---- */
    mov     w0, #0xa0
    strb    w0, [x8, #0x420]           /* IPRIORITYR[32] = 0xA0 */
    mov     w0, #0x40
    strb    w0, [x8, #0x421]           /* IPRIORITYR[33] = 0x40 (higher) */
    mov     w0, #0x3
    str     w0, [x8, #0x204]           /* ISPENDR: pend 32,33 */

    ldr     w1, [x9, #0x18]            /* HPPIR (non-destructive) */
    movz    x11, #241
    cmp     w1, #33
    b.ne    fail
    ldr     w1, [x9, #0x0c]           /* IAR: acks highest -> 33 */
    movz    x11, #242
    cmp     w1, #33
    b.ne    fail
    mov     w0, #33
    str     w0, [x9, #0x10]           /* EOIR 33 */
    ldr     w1, [x9, #0x0c]           /* IAR: next -> 32 */
    movz    x11, #243
    cmp     w1, #32
    b.ne    fail
    mov     w0, #32
    str     w0, [x9, #0x10]           /* EOIR 32 */
    ldr     w1, [x9, #0x0c]           /* IAR: none -> 1023 spurious */
    movz    x11, #244
    cmp     w1, #1023
    b.ne    fail

    /* ---- check 2: PMR strict-less-than boundary ---- */
    mov     w0, #0x80
    strb    w0, [x8, #0x420]           /* IPRIORITYR[32] = 0x80 */
    mov     w0, #1
    str     w0, [x8, #0x204]           /* pend 32 */
    mov     w0, #0x80
    str     w0, [x9, #0x04]            /* PMR = 0x80: 0x80 < 0x80 is false */
    ldr     w1, [x9, #0x18]
    movz    x11, #245
    cmp     w1, #1023
    b.ne    fail
    mov     w0, #0x81
    str     w0, [x9, #0x04]            /* PMR = 0x81: 0x80 < 0x81 -> eligible */
    ldr     w1, [x9, #0x18]
    movz    x11, #246
    cmp     w1, #32
    b.ne    fail
    mov     w0, #0xff
    str     w0, [x9, #0x04]            /* restore PMR */
    mov     w0, #1
    str     w0, [x8, #0x284]           /* ICPENDR: clear 32 */

    /* ---- check 3: ISACTIVER set by IAR, cleared by EOIR ---- */
    mov     w0, #1
    str     w0, [x8, #0x204]           /* pend 32 */
    ldr     w1, [x9, #0x0c]           /* IAR -> 32 (active[32]=1) */
    movz    x11, #247
    cmp     w1, #32
    b.ne    fail
    ldr     w1, [x8, #0x304]           /* ISACTIVER word1 */
    and     w1, w1, #1
    movz    x11, #248
    cmp     w1, #1
    b.ne    fail
    mov     w0, #32
    str     w0, [x9, #0x10]           /* EOIR 32 */
    ldr     w1, [x8, #0x304]
    and     w1, w1, #1
    movz    x11, #249
    cbnz    w1, fail

    /* ---- check 4: ICPENDR clears a pending interrupt ---- */
    mov     w0, #1
    str     w0, [x8, #0x204]           /* pend 32 */
    ldr     w1, [x8, #0x204]           /* ISPENDR read-back */
    and     w1, w1, #1
    movz    x11, #250
    cmp     w1, #1
    b.ne    fail
    mov     w0, #1
    str     w0, [x8, #0x284]           /* ICPENDR: clear */
    ldr     w1, [x8, #0x204]
    and     w1, w1, #1
    movz    x11, #251
    cbnz    w1, fail

    /* ---- check 5: ICENABLER gates eligibility ---- */
    mov     w0, #1
    str     w0, [x8, #0x204]           /* pend 32 */
    mov     w0, #1
    str     w0, [x8, #0x184]           /* ICENABLER word1: disable 32 */
    ldr     w1, [x9, #0x18]
    movz    x11, #252
    cmp     w1, #1023
    b.ne    fail
    mov     w0, #1
    str     w0, [x8, #0x104]           /* ISENABLER word1: re-enable 32 */
    ldr     w1, [x9, #0x18]
    movz    x11, #253
    cmp     w1, #32
    b.ne    fail
    mov     w0, #1
    str     w0, [x8, #0x284]           /* clear 32 */

    /* ---- check 6: ITARGETSR banking (PPI/SGI RO 0x01, SPI read-write) ---- */
    ldrb    w1, [x8, #0x810]           /* ITARGETSR[16] (intid<32) -> 0x01 RO */
    movz    x11, #254
    cmp     w1, #1
    b.ne    fail
    mov     w0, #1
    strb    w0, [x8, #0x820]           /* ITARGETSR[32] = 0x01 (SPI writable) */
    ldrb    w1, [x8, #0x820]
    movz    x11, #255
    cmp     w1, #1
    b.ne    fail

    /* ---- check 7: GICD_CTLR gates the whole distributor ---- */
    mov     w0, #1
    str     w0, [x8, #0x204]           /* pend 32 */
    str     wzr, [x8, #0x000]          /* GICD_CTLR = 0 */
    ldr     w1, [x9, #0x18]
    movz    x11, #256
    cmp     w1, #1023
    b.ne    fail
    mov     w0, #1
    str     w0, [x8, #0x000]           /* GICD_CTLR = 1 */
    ldr     w1, [x9, #0x18]
    movz    x11, #257
    cmp     w1, #32
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0
