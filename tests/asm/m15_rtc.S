/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M15 test: PL031 RTC match/alarm interrupt.
 *
 * run_tests.sh runs this one as `-bios <img>`, not `-bin`: the pl031 (like every
 * MMIO device) is created only when platform_build runs under -bios/-kernel.
 * So this executes from flash at 0x0, at EL1 with the MMU off, and uses physical
 * addresses directly for the pl031 at 0x09010000.
 *
 * Arm the alarm with RTCMR == the current count (so the match is due), unmask it
 * (RTCIMSC), spin past a machine_tick so pl031_update latches the match, and
 * confirm RTCMIS[0] and RTCRIS[0] are set. Then clear via RTCICR and confirm the
 * masked status drops. x0=0 on success, else a failing-check id. */

.text
.global _start
_start:
    movz    x9, #0x0901, lsl #16        /* 0x09010000 pl031 base */

    /* read current count (RTCDR @ 0x00), arm the alarm at that value (RTCMR) */
    ldr     w2, [x9, #0x00]
    str     w2, [x9, #0x04]            /* RTCMR = count -> due now, arms the alarm */
    mov     w0, #1
    str     w0, [x9, #0x10]            /* RTCIMSC = 1 (unmask the match interrupt) */

    /* spin > 1024 instructions so the run loop's machine_tick runs pl031_update */
    movz    x3, #0x2000                 /* 8192 iterations */
1:  subs    x3, x3, #1
    b.ne    1b

    /* RTCMIS @ 0x18 = RIS & IMSC; bit 0 must be set by the match */
    ldr     w1, [x9, #0x18]
    and     w1, w1, #1
    cmp     w1, #1
    b.ne    fail1

    /* RTCRIS @ 0x14 raw status bit 0 must be set too */
    ldr     w1, [x9, #0x14]
    and     w1, w1, #1
    cmp     w1, #1
    b.ne    fail2

    /* RTCICR @ 0x1c clears the raw status -> RTCMIS must drop to 0 */
    mov     w0, #1
    str     w0, [x9, #0x1c]
    ldr     w1, [x9, #0x18]
    and     w1, w1, #1
    cbnz    w1, fail3

    movz    x0, #0
    hlt     #0
fail1:
    movz    x0, #71
    hlt     #0
fail2:
    movz    x0, #72
    hlt     #0
fail3:
    movz    x0, #73
    hlt     #0
