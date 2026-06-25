/* ARM architected generic timer.
 *
 * By default the counter is driven by the retired-instruction count, NOT the
 * host wall clock: one CNTFRQ tick per executed instruction, plus a fast-forward
 * accumulated while the CPU is halted in WFI (see machine_wait_for_event). This
 * makes the timer fully deterministic -- the timer IRQ lands at the identical
 * instruction boundary on every run -- so timing-dependent boot failures become
 * reproducible. Set AE_RTCLOCK=1 to revert to the host monotonic clock (natural
 * wall-clock guest time, but non-reproducible IRQ placement). */
#include "../devices.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#define ull unsigned long long

static u64 host_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

/* Strong definition; overrides the weak fallback in sysreg.c. */
u64 gt_count(CPU *c, bool virt) {
    u64 ticks;
    if (g_rtclock) {
        u64 now = host_ns();
        if (c->cntpct_base == 0) c->cntpct_base = now;
        unsigned __int128 d = (unsigned __int128)(now - c->cntpct_base) * c->cntfrq;
        ticks = (u64)(d / 1000000000ULL);
    } else {
        ticks = c->icount + c->timer_skip;   /* deterministic: 1 tick / instruction */
    }
    return virt ? ticks - (u64)c->cntvoff : ticks;
}

static bool fires(u64 ctl, u64 count, u64 cval) {
    return (ctl & 1) && !(ctl & 2) && (count >= cval);   /* ENABLE && !IMASK && reached */
}

void timer_update(Machine *m) {
    CPU *c = &m->cpu;
    u64 pc = gt_count(c, false), vc = gt_count(c, true);
    int pf = fires(c->cntp_ctl, pc, c->cntp_cval);
    int vf = fires(c->cntv_ctl, vc, c->cntv_cval);
    if (g_dbg) {
        static int last_p = -1, last_v = -1;
        if (pf != last_p) { fprintf(stderr, "[timer] phys line=%d ctl=%llu cval=%llu cnt=%llu\n",
                            pf, (ull)c->cntp_ctl, (ull)c->cntp_cval, (ull)pc); last_p = pf; }
        if (vf != last_v) { fprintf(stderr, "[timer] virt line=%d ctl=%llu cval=%llu cnt=%llu\n",
                            vf, (ull)c->cntv_ctl, (ull)c->cntv_cval, (ull)vc); last_v = vf; }
    }
    gic_set_irq(m->gic, INTID_TIMER_PHYS, pf);
    gic_set_irq(m->gic, INTID_TIMER_VIRT, vf);
}

u64 timer_next_deadline_ns(Machine *m) {
    CPU *c = &m->cpu;
    u64 best = ~0ULL;
    u64 pc = gt_count(c, false), vc = gt_count(c, true);
    if ((c->cntp_ctl & 1) && !(c->cntp_ctl & 2)) {
        if (c->cntp_cval <= pc) return 0;
        unsigned __int128 dt = (unsigned __int128)(c->cntp_cval - pc) * 1000000000ULL / c->cntfrq;
        if ((u64)dt < best) best = (u64)dt;
    }
    if ((c->cntv_ctl & 1) && !(c->cntv_ctl & 2)) {
        if (c->cntv_cval <= vc) return 0;
        unsigned __int128 dt = (unsigned __int128)(c->cntv_cval - vc) * 1000000000ULL / c->cntfrq;
        if ((u64)dt < best) best = (u64)dt;
    }
    return best;
}

/* Deterministic-mode counterpart: ticks (= instructions) until the next armed,
 * unmasked timer fires; 0 if already due, ~0 if none armed. Used to fast-forward
 * the virtual clock across a WFI idle so the timer fires at a fixed icount. */
u64 timer_next_deadline_ticks(Machine *m) {
    CPU *c = &m->cpu;
    u64 best = ~0ULL;
    u64 pc = gt_count(c, false), vc = gt_count(c, true);
    if ((c->cntp_ctl & 1) && !(c->cntp_ctl & 2)) {
        u64 d = (c->cntp_cval <= pc) ? 0 : (c->cntp_cval - pc);
        if (d < best) best = d;
    }
    if ((c->cntv_ctl & 1) && !(c->cntv_ctl & 2)) {
        u64 d = (c->cntv_cval <= vc) ? 0 : (c->cntv_cval - vc);
        if (d < best) best = d;
    }
    return best;
}

ARMTimer *gtimer_create(Machine *m) {
    ARMTimer *t = calloc(1, sizeof(*t));
    t->cpu = &m->cpu;
    t->gic = m->gic;
    m->cpu.cntpct_base = host_ns();
    m->timer = t;
    return t;
}
