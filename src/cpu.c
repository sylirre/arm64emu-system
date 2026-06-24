/* CPU core: reset, single-step driver, condition codes, dump. */
#include "cpu.h"
#include "mmu.h"
#include "sysreg.h"
#include <stdio.h>
#include <string.h>

int g_trace = 0;
int g_singlestep = 0;
int g_dbg = 0;
int g_rtrace = 0;

void cpu_reset(CPU *c, u64 entry, unsigned reset_el) {
    struct Machine *m = c->m;
    memset(c, 0, sizeof(*c));
    c->m = m;
    c->pc = entry;
    c->el = reset_el;
    c->sp_sel = 1;                       /* SPSel=1 at reset (use SP_ELx) */
    c->daif = PS_D | PS_A | PS_I | PS_F; /* all masked at reset */
    c->nzcv = 0;
    c->cntfrq = 24000000ULL;             /* 24 MHz, matches virt DTB */
    /* MPIDR_EL1: bit31 RES1, single core affinity 0. */
    c->mpidr = (1ULL << 31);
    if (sysreg_init) sysreg_init(c);
}

bool cond_holds(CPU *c, unsigned cond) {
    bool N = (c->nzcv & PS_N) != 0;
    bool Z = (c->nzcv & PS_Z) != 0;
    bool C = (c->nzcv & PS_C) != 0;
    bool V = (c->nzcv & PS_V) != 0;
    bool r;
    switch (cond >> 1) {
        case 0: r = Z; break;                 /* EQ/NE */
        case 1: r = C; break;                 /* CS/CC */
        case 2: r = N; break;                 /* MI/PL */
        case 3: r = V; break;                 /* VS/VC */
        case 4: r = C && !Z; break;           /* HI/LS */
        case 5: r = (N == V); break;          /* GE/LT */
        case 6: r = (N == V) && !Z; break;    /* GT/LE */
        default: r = true; break;             /* AL/NV */
    }
    if ((cond & 1) && cond != 0xf) r = !r;
    return r;
}

StepResult cpu_step(CPU *c) {
    if (c->stop) return STEP_HALT;

    /* Pending IRQ delivery (single-CPU). FIQ handled the same way. */
    if (c->fiq_line && !(c->daif & PS_F)) {
        exception_take(c, EXC_FIQ, 0, 0, c->pc);
        return STEP_OK;
    }
    if (c->irq_line && !(c->daif & PS_I)) {
        exception_take(c, EXC_IRQ, 0, 0, c->pc);
        return STEP_OK;
    }

    if (c->halted) return STEP_OK;   /* WFI/WFE: caller waits for an event */

    u32 insn;
    if (!mem_ifetch(c, c->pc, &insn)) return STEP_OK;  /* fetch raised an abort */

    if (g_rtrace) {
        fprintf(stderr, "P%llx", (unsigned long long)c->pc);
        for (int i = 0; i < 31; i++) fprintf(stderr, " %llx", (unsigned long long)c->x[i]);
        fprintf(stderr, " S%llx\n", (unsigned long long)*cpu_cur_sp(c));
    }
    if (g_trace) {
        fprintf(stderr, "%016llx: %08x  [el%u nzcv=%c%c%c%c]\n",
                (unsigned long long)c->pc, insn, c->el,
                (c->nzcv & PS_N) ? 'N' : '.', (c->nzcv & PS_Z) ? 'Z' : '.',
                (c->nzcv & PS_C) ? 'C' : '.', (c->nzcv & PS_V) ? 'V' : '.');
    }

    c->cur_insn_pc = c->pc;
    c->pc += 4;
    exec_a64(c, insn);
    c->icount++;

    return c->stop ? STEP_HALT : STEP_OK;
}

void cpu_dump(CPU *c) {
    for (int i = 0; i < 31; i++) {
        fprintf(stderr, "x%-2d=%016llx%s", i, (unsigned long long)c->x[i],
                (i % 4 == 3) ? "\n" : "  ");
    }
    fprintf(stderr, "\nsp =%016llx  pc =%016llx  el=%u  spsel=%u\n",
            (unsigned long long)*cpu_cur_sp(c), (unsigned long long)c->pc,
            c->el, c->sp_sel);
    fprintf(stderr, "nzcv=%c%c%c%c daif=%c%c%c%c  icount=%llu\n",
            (c->nzcv & PS_N) ? 'N' : '.', (c->nzcv & PS_Z) ? 'Z' : '.',
            (c->nzcv & PS_C) ? 'C' : '.', (c->nzcv & PS_V) ? 'V' : '.',
            (c->daif & PS_D) ? 'D' : '.', (c->daif & PS_A) ? 'A' : '.',
            (c->daif & PS_I) ? 'I' : '.', (c->daif & PS_F) ? 'F' : '.',
            (unsigned long long)c->icount);
}
