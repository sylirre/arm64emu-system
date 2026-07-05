/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* PSCI over the SMC/HVC conduit (emulated firmware service). */
#include "../cpu.h"
#include <stdio.h>

#define PSCI_VERSION       0x84000000u
#define PSCI_CPU_SUSPEND64 0xc4000001u
#define PSCI_CPU_OFF       0x84000002u
#define PSCI_CPU_ON64      0xc4000003u
#define PSCI_AFFINITY_INFO64 0xc4000004u
#define PSCI_MIGRATE_INFO_TYPE 0x84000006u
#define PSCI_SYSTEM_OFF    0x84000008u
#define PSCI_SYSTEM_RESET  0x84000009u
#define PSCI_FEATURES      0x8400000au

#define PSCI_OK             0
#define PSCI_NOT_SUPPORTED  ((u64)-1)
#define PSCI_INVALID_PARAMS ((u64)-2)

bool smccc_conduit(CPU *c, bool is_hvc) {
    u32 fn = (u32)c->x[0];
    u64 ret;
    switch (fn) {
        case PSCI_VERSION:           ret = 0x00010000; break;       /* v1.0 */
        case PSCI_FEATURES: {
            u32 q = (u32)c->x[1];
            ret = (q == PSCI_CPU_ON64 || q == PSCI_SYSTEM_OFF ||
                   q == PSCI_SYSTEM_RESET || q == PSCI_VERSION ||
                   q == PSCI_CPU_SUSPEND64 ||
                   q == PSCI_AFFINITY_INFO64) ? PSCI_OK : PSCI_NOT_SUPPORTED;
            break;
        }
        case PSCI_CPU_SUSPEND64:
            /* Low-power suspend. We model only standby/retention: halt until an
             * interrupt (WFI semantics) and return success to the caller. */
            c->halted = true;
            ret = PSCI_OK;
            break;
        case PSCI_AFFINITY_INFO64:   ret = (c->x[1] == 0) ? 0 /*ON*/ : 1 /*OFF*/; break;
        case PSCI_MIGRATE_INFO_TYPE: ret = 2; break;                /* not present */
        case PSCI_CPU_ON64:          ret = PSCI_NOT_SUPPORTED; break; /* single core */
        case PSCI_CPU_OFF:           ret = PSCI_NOT_SUPPORTED; break;
        case PSCI_SYSTEM_OFF:
            fprintf(stderr, "\n[PSCI SYSTEM_OFF]\n");
            c->stop = true; return true;
        case PSCI_SYSTEM_RESET:
            fprintf(stderr, "\n[PSCI SYSTEM_RESET]\n");
            /* Warm reboot: stop the current run-loop pass; main() sees c->reset
             * and rebuilds machine state instead of exiting. */
            c->reset = true; c->stop = true; return true;
        default:
            ret = PSCI_NOT_SUPPORTED;
            break;
    }
    c->x[0] = ret;
    (void)is_hvc;
    return true;
}
