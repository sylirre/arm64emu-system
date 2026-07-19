/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M28 test: PL011 UART flags, interrupt-mask registers, and the TX interrupt
 * routed through the GICv2.
 *
 * run_tests.sh runs this as `--bios <img>` (the PL011 + GICv2 exist only under
 * platform_build). Executes from flash at 0x0, EL1, MMU off, driving the UART at
 * 0x09000000 and the GIC at 0x08000000 / 0x08010000. DAIF.I stays masked; the
 * test polls the GIC (INTID_UART = 33) directly.
 *
 * The PL011's TXRIS is always asserted (the TX FIFO is bottomless here), so
 * unmasking TXIM (IMSC bit5) raises INTID 33, observable through GICC_IAR. RX and
 * the RX interrupt are NOT exercised: the harness stdin is non-tty so
 * tty_getchar never yields a byte, and DR (TX to host stdout) is left unasserted.
 * x0=0 on success, else a failing-check id. */
.text
.global _start
_start:
    movz    x10, #0x0900, lsl #16       /* PL011 base 0x09000000 */
    movz    x8, #0x0800, lsl #16        /* GICD base */
    movz    x9, #0x0801, lsl #16        /* GICC base */

    /* enable GIC, unmask priorities, enable + prioritize INTID 33 (UART SPI) */
    mov     w0, #1
    str     w0, [x8, #0x000]           /* GICD_CTLR = 1 */
    str     w0, [x9, #0x000]           /* GICC_CTLR = 1 */
    mov     w0, #0xff
    str     w0, [x9, #0x004]           /* GICC_PMR = 0xff */
    mov     w0, #0x80
    strb    w0, [x8, #0x421]           /* IPRIORITYR[33] = 0x80 */
    mov     w0, #0x2
    str     w0, [x8, #0x104]           /* ISENABLER word1: enable INTID 33 */

    /* ---- check 1: FR reports the TX FIFO empty (TXFE) ---- */
    ldr     w1, [x10, #0x18]           /* FR */
    and     w2, w1, #0x80               /* TXFE (bit7) */
    movz    x11, #320
    cbz     w2, fail

    /* ---- check 2: IMSC/RIS/MIS round-trip ---- */
    mov     w0, #0x20                    /* TXIM (bit5) */
    str     w0, [x10, #0x38]           /* IMSC = 0x20 */
    ldr     w1, [x10, #0x38]           /* IMSC read-back */
    movz    x11, #321
    cmp     w1, #0x20
    b.ne    fail
    ldr     w1, [x10, #0x3c]           /* RIS: TXRIS (bit5) always set */
    and     w1, w1, #0x20
    movz    x11, #322
    cmp     w1, #0x20
    b.ne    fail
    ldr     w1, [x10, #0x40]           /* MIS = RIS & IMSC */
    movz    x11, #323
    cmp     w1, #0x20
    b.ne    fail

    /* ---- check 3: the TX interrupt reaches the GIC ---- */
    ldr     w1, [x9, #0x18]            /* HPPIR */
    movz    x11, #324
    cmp     w1, #33
    b.ne    fail
    ldr     w1, [x9, #0x0c]            /* IAR -> 33 */
    movz    x11, #325
    cmp     w1, #33
    b.ne    fail
    str     wzr, [x10, #0x38]          /* IMSC = 0: deassert the UART line */
    mov     w0, #33
    str     w0, [x9, #0x10]            /* EOIR 33 */
    ldr     w1, [x9, #0x0c]            /* IAR -> 1023 spurious */
    movz    x11, #326
    cmp     w1, #1023
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0
