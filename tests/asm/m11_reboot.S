/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* System-reboot (PSCI SYSTEM_RESET) self-test.
 *
 * First boot: stash a marker in persistent RAM, then issue PSCI SYSTEM_RESET.
 * A working warm reboot restarts execution at _start with registers cleared but
 * RAM contents intact, so the second pass sees the marker and exits x0=0 (HLT).
 * If reboot were unimplemented the emulator would terminate at the SMC and never
 * reach a HLT, so run_tests.sh would see no x0=0x0 and report failure.
 *
 * The marker lives at 0x40080000: far enough past the tiny loaded image that the
 * post-reset -bin reload does not overwrite it, and warm reset does not clear RAM.
 */

.text
.global _start
_start:
    movz    x2, #0x4008, lsl #16        /* x2 = 0x40080000 persistent scratch */
    ldr     x1, [x2]
    cbnz    x1, second_boot             /* marker present -> this is the reboot */

    /* ---- first boot: set marker, then request PSCI SYSTEM_RESET ---- */
    movz    x1, #0xABCD
    str     x1, [x2]
    movz    x0, #0x8400, lsl #16
    movk    x0, #0x0009                 /* x0 = 0x84000009  PSCI SYSTEM_RESET */
    smc     #0
    movz    x0, #1                      /* reset must not return; if it did, fail */
    hlt     #0

    /* ---- second boot: confirm RAM survived and registers were cleared ---- */
second_boot:
    movz    x3, #0xABCD
    cmp     x1, x3                      /* marker must be exactly what we wrote */
    b.ne    fail
    movz    x0, #0
    hlt     #0

fail:
    movz    x0, #2
    hlt     #0
