/* AArch64 stage-1 address translation (EL1&0 regime, 4 KB granule) with a
 * software TLB, plus typed guest memory accesses. */
#include "mmu.h"
#include "machine.h"
#include "esr.h"
#include <string.h>

/* ---------- software TLB (single CPU) ---------- */
#define TLB_ENTRIES 4096
typedef struct {
    u64 va_page;     /* 4 KB-aligned VA, +1 in low bit used as "valid" via tag */
    u64 pa_page;     /* 4 KB-aligned PA */
    u8  valid;
    u8  ap;          /* AP[2:1] */
    u8  uxn, pxn;    /* execute-never bits */
    u8  el0;         /* regime/EL marker to avoid cross-EL reuse */
} TLBEntry;
static TLBEntry tlb[TLB_ENTRIES];

void tlb_flush_all(void) { memset(tlb, 0, sizeof(tlb)); }

static inline unsigned tlb_idx(u64 va) { return (unsigned)((va >> 12) & (TLB_ENTRIES - 1)); }

/* ---------- fault injection ---------- */
static bool raise_abort(CPU *c, u64 va, AccType acc, unsigned fsc) {
    bool same = (c->el >= 1);
    if (acc == ACC_EXEC)
        cpu_raise_sync(c, esr_make(same ? EC_IABORT_SAME : EC_IABORT_LOWER, fsc & 0x3f), va);
    else
        cpu_raise_sync(c, esr_make(same ? EC_DABORT_SAME : EC_DABORT_LOWER,
                                   iss_dabort(acc == ACC_WRITE, fsc)), va);
    return false;
}

/* Check AP/XN permissions for an access; returns true if permitted. */
static bool perm_ok(CPU *c, AccType acc, u8 ap, u8 uxn, u8 pxn) {
    bool el0 = (c->el == 0);
    bool can_read, can_write, can_exec;
    switch (ap & 3) {
        case 0: can_read = !el0; can_write = !el0; break;            /* EL1 RW, EL0 none */
        case 1: can_read = true; can_write = true; break;            /* RW/RW */
        case 2: can_read = !el0; can_write = false; break;           /* EL1 RO, EL0 none */
        default: can_read = true; can_write = false; break;          /* RO/RO */
    }
    can_exec = el0 ? !uxn : !pxn;
    if (acc == ACC_READ)  return can_read;
    if (acc == ACC_WRITE) return can_write;
    return can_exec && can_read;
}

/* Translate a 4 KB page; fills *pa_page and perms. Returns FSC (0 == success). */
static unsigned walk(CPU *c, u64 va, u64 *pa_page, u8 *ap, u8 *uxn, u8 *pxn) {
    Machine *m = c->m;
    u64 tcr = c->tcr[1];
    u64 ttbr;
    unsigned txsz;
    if ((s64)va >= 0) { ttbr = c->ttbr0[1]; txsz = tcr & 0x3f; }
    else              { ttbr = c->ttbr1[1]; txsz = (tcr >> 16) & 0x3f; }
    if (txsz < 16) txsz = 16;
    if (txsz > 39) txsz = 39;
    unsigned va_size = 64 - txsz;

    /* starting level for 4 KB granule */
    unsigned n = (va_size - 12 + 8) / 9;
    int level = (int)(4 - n);
    if (level < 0) level = 0;

    u64 table = ttbr & 0x0000FFFFFFFFF000ULL;
    u8 r_ap = 0, r_uxn = 0, r_pxn = 0;

    for (; level <= 3; level++) {
        unsigned shift = 12 + 9 * (3 - level);
        unsigned idx = (unsigned)((va >> shift) & 0x1ff);
        u64 desc = phys_read(m, table + (u64)idx * 8, 8);
        if (m->last_bus_status != BUS_OK) return FSC_EXTERNAL;
        if ((desc & 1) == 0) return FSC_TRANS_L0 + level;          /* invalid */

        /* accumulate hierarchical AP/XN from table descriptors is ignored (rare) */
        if ((desc & 3) == 1) {                                     /* block */
            if (level == 3) return FSC_TRANS_L0 + level;           /* reserved */
            u64 blk_mask = (1ULL << shift) - 1;
            u64 oa = desc & 0x0000FFFFFFFFF000ULL & ~blk_mask;
            *pa_page = (oa | (va & blk_mask)) & ~0xfffULL;
            r_ap = (desc >> 6) & 3; r_pxn = (desc >> 53) & 1; r_uxn = (desc >> 54) & 1;
            if (((desc >> 10) & 1) == 0) return FSC_ACCESS_L0 + level; /* AF */
            *ap = r_ap; *uxn = r_uxn; *pxn = r_pxn;
            return 0;
        }
        if ((desc & 3) == 3) {
            if (level == 3) {                                      /* page */
                *pa_page = desc & 0x0000FFFFFFFFF000ULL;
                r_ap = (desc >> 6) & 3; r_pxn = (desc >> 53) & 1; r_uxn = (desc >> 54) & 1;
                if (((desc >> 10) & 1) == 0) return FSC_ACCESS_L0 + level;
                *ap = r_ap; *uxn = r_uxn; *pxn = r_pxn;
                return 0;
            }
            table = desc & 0x0000FFFFFFFFF000ULL;                  /* next table */
            continue;
        }
        return FSC_TRANS_L0 + level;
    }
    return FSC_TRANS_L3;
}

bool mmu_translate(CPU *c, u64 va, AccType acc, u64 *pa_out) {
    /* MMU off -> identity map. SCTLR_EL1.M is bit 0. */
    if ((c->sctlr[1] & 1) == 0) { *pa_out = va; return true; }

    unsigned i = tlb_idx(va);
    TLBEntry *e = &tlb[i];
    u64 page = va & ~0xfffULL;
    if (e->valid && e->va_page == page && e->el0 == (c->el == 0)) {
        if (!perm_ok(c, acc, e->ap, e->uxn, e->pxn)) {
            raise_abort(c, va, acc, FSC_PERM_L3);
            return false;
        }
        *pa_out = e->pa_page | (va & 0xfff);
        return true;
    }

    u64 pa_page; u8 ap, uxn, pxn;
    unsigned fsc = walk(c, va, &pa_page, &ap, &uxn, &pxn);
    if (fsc) { raise_abort(c, va, acc, fsc); return false; }

    e->valid = 1; e->va_page = page; e->pa_page = pa_page;
    e->ap = ap; e->uxn = uxn; e->pxn = pxn; e->el0 = (c->el == 0);

    if (!perm_ok(c, acc, ap, uxn, pxn)) { raise_abort(c, va, acc, FSC_PERM_L3); return false; }
    *pa_out = pa_page | (va & 0xfff);
    return true;
}

/* ---------- typed accesses ---------- */
bool mem_read(CPU *c, u64 va, unsigned size, u64 *out) {
    u64 pa;
    if (!mmu_translate(c, va, ACC_READ, &pa)) return false;
    u64 v = phys_read(c->m, pa, size);
    if (c->m->last_bus_status != BUS_OK) return raise_abort(c, va, ACC_READ, FSC_EXTERNAL);
    *out = v;
    return true;
}

bool mem_write(CPU *c, u64 va, unsigned size, u64 val) {
    u64 pa;
    if (!mmu_translate(c, va, ACC_WRITE, &pa)) return false;
    phys_write(c->m, pa, size, val);
    if (c->m->last_bus_status != BUS_OK) return raise_abort(c, va, ACC_WRITE, FSC_EXTERNAL);
    return true;
}

bool mem_ifetch(CPU *c, u64 va, u32 *insn_out) {
    u64 pa;
    if (!mmu_translate(c, va, ACC_EXEC, &pa)) return false;
    u64 v = phys_read(c->m, pa, 4);
    if (c->m->last_bus_status != BUS_OK) return raise_abort(c, va, ACC_EXEC, FSC_EXTERNAL);
    *insn_out = (u32)v;
    return true;
}

bool mem_read128(CPU *c, u64 va, V128 *out) {
    u64 lo, hi;
    if (!mem_read(c, va, 8, &lo)) return false;
    if (!mem_read(c, va + 8, 8, &hi)) return false;
    out->d[0] = lo; out->d[1] = hi;
    return true;
}

bool mem_write128(CPU *c, u64 va, const V128 *val) {
    if (!mem_write(c, va, 8, val->d[0])) return false;
    if (!mem_write(c, va + 8, 8, val->d[1])) return false;
    return true;
}
