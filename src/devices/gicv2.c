/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* GICv2: distributor (GICD) + CPU interface (GICC). Single CPU. */
#include "../devices.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SPURIOUS 1023

/* Pick the highest-priority eligible interrupt (lowest priority value, lowest
 * INTID as tie-break). Returns SPURIOUS if none. */
static int gic_best(GIC *g) {
    if (!(g->d_ctlr & 1) || !(g->c_ctlr & 1)) return SPURIOUS;
    int best = SPURIOUS, bestpri = 256;
    for (int i = 0; i < GIC_NUM_IRQS; i++) {
        if (!g->enabled[i] || !g->pending[i] || g->active[i]) continue;
        if (g->priority[i] >= g->pmr) continue;        /* masked */
        if (g->priority[i] < bestpri) { bestpri = g->priority[i]; best = i; }
    }
    return best;
}

void gic_update(GIC *g) {
    int best = gic_best(g);
    bool now = (best != SPURIOUS);
    if (g_dbg && now && !g->cpu->irq_line)
        fprintf(stderr, "[gic] assert irq best=%d pri=%d pmr=%d dctlr=%x cctlr=%x\n",
                best, g->priority[best], g->pmr, g->d_ctlr, g->c_ctlr);
    g->cpu->irq_line = now;
}

void gic_set_irq(GIC *g, int intid, int level) {
    if (intid < 0 || intid >= GIC_NUM_IRQS) return;
    if (g->cfg_edge[intid]) {
        if (level && !g->line[intid]) g->pending[intid] = true;   /* rising edge */
    } else {
        g->pending[intid] = level != 0;                            /* level */
    }
    g->line[intid] = level != 0;
    gic_update(g);
}

/* ---- distributor ---- */
static u64 gicd_read(void *opaque, u64 off, unsigned size) {
    GIC *g = opaque;
    if (off == 0x000) return g->d_ctlr;
    if (off == 0x004) return (GIC_NUM_IRQS / 32 - 1) | (0 << 5);    /* TYPER */
    if (off == 0x008) return 0x0200043b;                           /* IIDR */
    if (off >= 0x100 && off < 0x180) {                             /* ISENABLER */
        unsigned base = (off - 0x100) * 8; u32 v = 0;
        for (int i = 0; i < 32; i++) if (g->enabled[base + i]) v |= 1u << i;
        return v;
    }
    if (off >= 0x180 && off < 0x200) {                             /* ICENABLER (rd == ISENABLER) */
        unsigned base = (off - 0x180) * 8; u32 v = 0;
        for (int i = 0; i < 32; i++) if (g->enabled[base + i]) v |= 1u << i;
        return v;
    }
    if (off >= 0x200 && off < 0x280) {                             /* ISPENDR */
        unsigned base = (off - 0x200) * 8; u32 v = 0;
        for (int i = 0; i < 32; i++) if (g->pending[base + i]) v |= 1u << i;
        return v;
    }
    if (off >= 0x280 && off < 0x300) {                             /* ICPENDR */
        unsigned base = (off - 0x280) * 8; u32 v = 0;
        for (int i = 0; i < 32; i++) if (g->pending[base + i]) v |= 1u << i;
        return v;
    }
    if (off >= 0x300 && off < 0x380) {                             /* ISACTIVER */
        unsigned base = (off - 0x300) * 8; u32 v = 0;
        for (int i = 0; i < 32; i++) if (g->active[base + i]) v |= 1u << i;
        return v;
    }
    if (off >= 0x400 && off < 0x800) {                             /* IPRIORITYR (byte each) */
        unsigned base = off - 0x400; u64 v = 0;
        for (unsigned i = 0; i < size; i++) v |= (u64)g->priority[base + i] << (i * 8);
        return v;
    }
    if (off >= 0x800 && off < 0xc00) {                             /* ITARGETSR */
        unsigned base = off - 0x800; u64 v = 0;
        for (unsigned i = 0; i < size; i++) v |= (u64)g->target[base + i] << (i * 8);
        return v;
    }
    if (off >= 0xc00 && off < 0xd00) {                             /* ICFGR (2 bits each) */
        unsigned base = (off - 0xc00) * 4; u32 v = 0;
        for (int i = 0; i < 16; i++) if (g->cfg_edge[base + i]) v |= 2u << (i * 2);
        return v;
    }
    return 0;
}

static void gicd_write(void *opaque, u64 off, unsigned size, u64 val) {
    GIC *g = opaque;
    if (off == 0x000) { g->d_ctlr = (u32)val; gic_update(g); return; }
    if (off >= 0x100 && off < 0x180) {                             /* ISENABLER */
        unsigned base = (off - 0x100) * 8;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) g->enabled[base + i] = true;
        gic_update(g); return;
    }
    if (off >= 0x180 && off < 0x200) {                             /* ICENABLER */
        unsigned base = (off - 0x180) * 8;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) g->enabled[base + i] = false;
        gic_update(g); return;
    }
    if (off >= 0x200 && off < 0x280) {                             /* ISPENDR */
        unsigned base = (off - 0x200) * 8;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) g->pending[base + i] = true;
        gic_update(g); return;
    }
    if (off >= 0x280 && off < 0x300) {                             /* ICPENDR */
        unsigned base = (off - 0x280) * 8;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) g->pending[base + i] = false;
        gic_update(g); return;
    }
    if (off >= 0x300 && off < 0x380) {                             /* ISACTIVER */
        unsigned base = (off - 0x300) * 8;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) g->active[base + i] = true;
        return;
    }
    if (off >= 0x380 && off < 0x400) {                             /* ICACTIVER */
        unsigned base = (off - 0x380) * 8;
        for (int i = 0; i < 32; i++) if (val & (1u << i)) g->active[base + i] = false;
        gic_update(g); return;
    }
    if (off >= 0x400 && off < 0x800) {                             /* IPRIORITYR */
        unsigned base = off - 0x400;
        for (unsigned i = 0; i < size; i++) g->priority[base + i] = (val >> (i * 8)) & 0xff;
        gic_update(g); return;
    }
    if (off >= 0x800 && off < 0xc00) {                             /* ITARGETSR */
        unsigned base = off - 0x800;
        for (unsigned i = 0; i < size; i++) g->target[base + i] = (val >> (i * 8)) & 0xff;
        return;
    }
    if (off >= 0xc00 && off < 0xd00) {                             /* ICFGR */
        unsigned base = (off - 0xc00) * 4;
        for (int i = 0; i < 16; i++) g->cfg_edge[base + i] = ((val >> (i * 2)) & 2) != 0;
        return;
    }
    /* SGIR etc: ignore (single CPU) */
}

/* ---- CPU interface ---- */
static u64 gicc_read(void *opaque, u64 off, unsigned size) {
    GIC *g = opaque;
    switch (off) {
        case 0x00: return g->c_ctlr;                  /* CTLR */
        case 0x04: return g->pmr;                     /* PMR */
        case 0x08: return 0;                          /* BPR */
        case 0x0c: {                                  /* IAR */
            int best = gic_best(g);
            if (best != SPURIOUS) {
                g->active[best] = true;
                if (g->cfg_edge[best]) g->pending[best] = false;
                if (g_dbg) fprintf(stderr, "[gic] IAR ack intid=%d\n", best);
                gic_update(g);
            }
            return best;
        }
        case 0x14: return 0;                          /* RPR */
        case 0x18: return gic_best(g);                /* HPPIR */
        case 0xfc: return 0x0202143b;                 /* IIDR */
        default: return 0;
    }
}

static void gicc_write(void *opaque, u64 off, unsigned size, u64 val) {
    GIC *g = opaque;
    switch (off) {
        case 0x00: g->c_ctlr = (u32)val; gic_update(g); break;        /* CTLR */
        case 0x04: g->pmr = (u8)val; gic_update(g); break;            /* PMR */
        case 0x10:                                                    /* EOIR */
        case 0x1000: {
            int id = val & 0x3ff;
            if (id < GIC_NUM_IRQS) g->active[id] = false;
            gic_update(g);
            break;
        }
        default: break;
    }
}

GIC *gic_create(Machine *m) {
    GIC *g = calloc(1, sizeof(*g));
    g->cpu = &m->cpu;
    g->pmr = 0;                /* reset masks all */
    machine_add_device(m, GICD_BASE, 0x10000, gicd_read, gicd_write, g, "gicd");
    machine_add_device(m, GICC_BASE, 0x10000, gicc_read, gicc_write, g, "gicc");
    m->gic = g;
    return g;
}

/* Return the distributor/CPU interface to power-on state (system reset). Clears
 * all enable/pending/active/line/config state so stale interrupts from the prior
 * boot don't leak into the rebooted machine; keeps the CPU backpointer. */
void gic_reset(GIC *g) {
    CPU *cpu = g->cpu;
    memset(g, 0, sizeof(*g));
    g->cpu = cpu;
    g->pmr = 0;
}
