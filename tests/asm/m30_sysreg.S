/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M30 test: system-register identification + MSR/MRS round-trip.
 *
 * A flat --bin test (EL1, MMU off). First asserts the ID_AA64* / MIDR / CTR /
 * DCZID feature registers read back exactly the advertised constants (sysreg.c
 * do_mrs) — a drift here silently changes what features the guest kernel probes.
 * Two unallocated ID slots are checked to read 0. CurrentEL must report EL1.
 * Then a batch of read-write control/translation registers is round-tripped
 * (write a pattern, read it back) to confirm the banked state is wired through
 * do_msr/do_mrs. x0=0 on success, else a failing-check id.
 *
 * NB: this flat --bin run has no platform devices, so it must not write any
 * CRn==14 register (CNTKCTL/CNT*): do_msr re-runs timer_update on every CRn==14
 * write, which dereferences the (absent) GIC. The generic-timer registers are
 * covered under --bios by m25_timer instead. */
.text
.global _start

.macro RT reg, val, id
    ldr     x0, =\val
    msr     \reg, x0
    mrs     x1, \reg
    movz    x11, #\id
    cmp     x0, x1
    b.ne    fail
.endm

_start:
    /* ---- ID / feature registers: exact advertised values ---- */
    mrs     x1, midr_el1
    ldr     x9, =0x411fd070
    movz    x11, #340
    cmp     x1, x9
    b.ne    fail

    mrs     x1, ctr_el0
    ldr     x9, =0x8444c004
    movz    x11, #341
    cmp     x1, x9
    b.ne    fail

    mrs     x1, dczid_el0
    movz    x11, #342
    cmp     x1, #4
    b.ne    fail

    mrs     x1, id_aa64pfr0_el1
    ldr     x9, =0x110022
    movz    x11, #343
    cmp     x1, x9
    b.ne    fail

    mrs     x1, id_aa64dfr0_el1
    ldr     x9, =0x10305106
    movz    x11, #344
    cmp     x1, x9
    b.ne    fail

    mrs     x1, id_aa64isar0_el1
    ldr     x9, =0x21100110212120
    movz    x11, #345
    cmp     x1, x9
    b.ne    fail

    mrs     x1, id_aa64isar1_el1
    ldr     x9, =0x211000
    movz    x11, #346
    cmp     x1, x9
    b.ne    fail

    mrs     x1, id_aa64isar2_el1
    ldr     x9, =0x10000
    movz    x11, #347
    cmp     x1, x9
    b.ne    fail

    mrs     x1, id_aa64mmfr0_el1
    ldr     x9, =0x0F001124
    movz    x11, #348
    cmp     x1, x9
    b.ne    fail

    /* unallocated ID slots read as 0 */
    mrs     x1, id_aa64pfr1_el1
    movz    x11, #349
    cbnz    x1, fail
    mrs     x1, id_aa64mmfr1_el1
    movz    x11, #350
    cbnz    x1, fail

    /* CurrentEL == EL1 (1 << 2) */
    mrs     x1, currentel
    movz    x11, #351
    cmp     x1, #4
    b.ne    fail

    /* ---- read-write control/translation registers: round-trip ---- */
    RT      tpidr_el0,     0x1111222233334444, 352
    RT      tpidr_el1,     0x5555666677778888, 353
    RT      contextidr_el1, 0x00000000abcd1234, 354
    RT      mair_el1,      0x00000000ff4400bb, 355
    RT      amair_el1,     0x0f1e2d3c4b5a6978, 356
    RT      ttbr0_el1,     0x0000000040091000, 357
    RT      ttbr1_el1,     0x00000000400a2000, 358
    RT      tcr_el1,       0x0000000280803519, 359
    RT      par_el1,       0x00000000fedcba98, 360
    RT      sp_el0,        0x0000000040300000, 361
    RT      vbar_el1,      0x0000000040010000, 362

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0
