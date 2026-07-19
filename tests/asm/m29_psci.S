/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M29 test: PSCI conduit functions over HVC (the virt DTB advertises
 * method="hvc").
 *
 * A flat --bin test: HVC from EL1 reaches smccc_conduit directly (no exception
 * vector). Checks the query/return values: VERSION, PSCI_FEATURES for the
 * supported and an unsupported function, AFFINITY_INFO for the present/absent
 * CPU, and MIGRATE_INFO_TYPE.
 *
 * CPU_SUSPEND64 is NOT invoked (it sets c->halted with no wake source in a flat
 * run, and halted short-circuits cpu_step before the next instruction, so no
 * following assert could run); SYSTEM_OFF / SYSTEM_RESET are NOT invoked (they
 * stop/reboot before the HLT dump). Their support is verified via PSCI_FEATURES
 * instead. x0=0 on success, else a failing-check id. */
.text
.global _start
_start:
    /* ---- PSCI_VERSION -> 0x0001_0000 (v1.0) ---- */
    ldr     x0, =0x84000000
    hvc     #0
    ldr     x9, =0x00010000
    movz    x11, #330
    cmp     x0, x9
    b.ne    fail

    /* ---- PSCI_FEATURES(SYSTEM_RESET) -> PSCI_OK (0) ---- */
    ldr     x0, =0x8400000a
    ldr     x1, =0x84000009
    hvc     #0
    movz    x11, #331
    cbnz    x0, fail

    /* ---- PSCI_FEATURES(SYSTEM_OFF) -> PSCI_OK ---- */
    ldr     x0, =0x8400000a
    ldr     x1, =0x84000008
    hvc     #0
    movz    x11, #332
    cbnz    x0, fail

    /* ---- PSCI_FEATURES(CPU_SUSPEND64) -> PSCI_OK ---- */
    ldr     x0, =0x8400000a
    ldr     x1, =0xc4000001
    hvc     #0
    movz    x11, #333
    cbnz    x0, fail

    /* ---- PSCI_FEATURES(CPU_OFF, unsupported) -> NOT_SUPPORTED (-1) ---- */
    ldr     x0, =0x8400000a
    ldr     x1, =0x84000002
    hvc     #0
    movz    x11, #334
    cmn     x0, #1                       /* Z set iff x0 == -1 */
    b.ne    fail

    /* ---- PSCI_AFFINITY_INFO(cpu0) -> 0 (ON) ---- */
    ldr     x0, =0xc4000004
    movz    x1, #0
    hvc     #0
    movz    x11, #335
    cbnz    x0, fail

    /* ---- PSCI_AFFINITY_INFO(cpu1) -> 1 (absent) ---- */
    ldr     x0, =0xc4000004
    movz    x1, #1
    hvc     #0
    movz    x11, #336
    cmp     x0, #1
    b.ne    fail

    /* ---- PSCI_MIGRATE_INFO_TYPE -> 2 (Trusted OS not present) ---- */
    ldr     x0, =0x84000006
    hvc     #0
    movz    x11, #337
    cmp     x0, #2
    b.ne    fail

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0
