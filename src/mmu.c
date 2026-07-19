/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* AArch64 stage-1 address translation (EL1&0 regime, 4 KB granule) with a
 * software TLB, plus typed guest memory accesses. */
#include "mmu.h"
#include "machine.h"
#include "esr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------- software TLB (single CPU) ---------- */
#define TLB_ENTRIES 4096
typedef struct {
    u64 va_page;     /* 4 KB-aligned VA, +1 in low bit used as "valid" via tag */
    u64 pa_page;     /* 4 KB-aligned PA */
    u32 gen;         /* g_tlb_gen at fill time; stale generation = invalid */
    u8  valid;
    u8  ap;          /* AP[2:1] */
    u8  uxn, pxn;    /* execute-never bits */
    u8  el0;         /* regime/EL marker to avoid cross-EL reuse */
} TLBEntry;
static TLBEntry tlb[TLB_ENTRIES];

/* Instruction-fetch fast-path caches, [el == 0] (single CPU, like the TLB). */
FetchCache g_fcache[2];

/* Data-access host-pointer D-TLB (layout contract in mmu.h). */
DTlbEnt g_dtlb[DTLB_SIZE];
u32 g_tlb_gen = 1;
const u8 *g_jit_code_bitmap;

/* Union range of all block-mapped (2 MB/1 GB) translations cached since the
 * last full flush, with the TBI tag byte stripped (accesses may carry tags,
 * TLBI VAs never do). lp_base == ~0 means none: no page-aligned VA matches
 * under the ~0 mask. A VA-form TLBI inside this range must escalate to a full
 * flush because the TLB holds the block as many 4 KB fragments (see
 * tlb_flush_page). Growing the union keeps it conservative: kernel linear-map
 * blocks live in high TTBR1 VAs, so the hot user-space per-page TLBIs
 * (COW/unmap) stay out of it unless the guest maps user hugepages. */
static u64 lp_base = ~0ULL, lp_mask = ~0ULL;
#define VA_NOTAG(va) ((va) & 0x00FFFFFFFFFFFFFFULL)

static inline unsigned tlb_idx(u64 va) { return (unsigned)((va >> 12) & (TLB_ENTRIES - 1)); }

static void lp_record(u64 va, unsigned blk_shift) {
    u64 bmask = ~((1ULL << blk_shift) - 1) & 0x00FFFFFFFFFFFFFFULL;
    u64 bbase = VA_NOTAG(va) & bmask;
    if (lp_base == ~0ULL) { lp_base = bbase; lp_mask = bmask; return; }
    u64 m = lp_mask & bmask;
    while ((lp_base & m) != (bbase & m)) m <<= 1;   /* smallest covering range */
    lp_mask = m;
    lp_base &= m;
}

/* O(1): bumping the generation invalidates every TLB and D-TLB entry (both
 * carry the generation in their tags). Boot flushes this on every TLBI and
 * TTBR/TCR/SCTLR write — ~1600 times per 4M instructions — so the previous
 * full memset of the 4096-entry TLB was ~14% of boot time. */
void tlb_flush_all(void) {
    ++g_tlb_gen;
    if ((g_tlb_gen & 0x3ff) == 0) {
        /* The D-TLB tag holds only 10 generation bits: purge on slice wrap so
         * an entry from 1024 flushes ago cannot alias the new generation. */
        memset(g_dtlb, 0, sizeof(g_dtlb));
        if (g_tlb_gen == 0) {           /* full u32 wrap: purge the TLB too */
            memset(tlb, 0, sizeof(tlb));
            g_tlb_gen = 1;
        }
    }
    lp_base = lp_mask = ~0ULL;   /* every fragment died with the generation */
    g_fcache[0].host = g_fcache[1].host = NULL;   /* force a re-translate */
}

u32 g_tlbi_va_seq;

void tlb_flush_page(u64 va) {
    if ((VA_NOTAG(va) & lp_mask) == lp_base) { tlb_flush_all(); return; }
    /* One direct-mapped slot per structure can hold this page: clear by index,
     * no tag compare (a TBI-tagged fill of the same page carries a different
     * tag; evicting an unrelated collider is just a refill). A zeroed D-TLB
     * entry has no R/W bits, so it can never fast-path. */
    tlb[tlb_idx(va)].valid = 0;
    DTlbEnt *de = &g_dtlb[(va >> 12) & (DTLB_SIZE - 1)];
    de->tag = 0; de->pte = 0;
    g_fcache[0].host = g_fcache[1].host = NULL;
    ++g_tlbi_va_seq;
}

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

/* Check AP/XN permissions for an access at an explicit privilege level;
 * returns true if permitted. el0 selects the EL0 view (used directly by the
 * LDTR/STTR unprivileged override and the AT S1E0* probes). */
static bool perm_ok_el(AccType acc, u8 ap, u8 uxn, u8 pxn, bool el0) {
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

/* The privilege an access is checked against: current EL, except when the
 * interpreter is running an LDTR/STTR (ldst_unpriv is set only around that
 * one data access in decode.c), which is checked as EL0. */
static inline bool acc_el0(const CPU *c) {
    return c->el == 0 || c->ldst_unpriv;
}

static bool perm_ok(CPU *c, AccType acc, u8 ap, u8 uxn, u8 pxn) {
    return perm_ok_el(acc, ap, uxn, pxn, acc_el0(c));
}

/* Translate a 4 KB page; fills *pa_page and perms, plus the leaf descriptor's
 * AttrIndx[4:2] and SH[9:8] when attridx/sh are non-NULL (AT needs them for
 * PAR_EL1; the access paths don't). When blk_shift is non-NULL it reports the
 * leaf granularity: 0 for a level-3 page, else the block's bit width (21/30 —
 * the TLB fill path records these for tlb_flush_page's escalation range).
 * Returns FSC (0 == success). */
static unsigned walk(CPU *c, u64 va, u64 *pa_page, u8 *ap, u8 *uxn, u8 *pxn,
                     u8 *attridx, u8 *sh, unsigned *blk_shift) {
    Machine *m = c->m;
    u64 tcr = c->tcr[1];
    /* Regime select by bit 55 (== bit 63 for any well-formed address; the bits
     * in between are validated below). Honor TBIx: when the top-byte-ignore bit
     * for the chosen regime is set, bits [63:56] are a tag and are excluded from
     * both the range check and the walk (AArch64 AddrTop = 55). This keeps
     * tagged EL0 pointers (Linux sets TBI0) on TTBR0 instead of misrouting them. */
    unsigned sel  = (unsigned)((va >> 55) & 1);           /* 0 = TTBR0, 1 = TTBR1 */
    unsigned tbi  = sel ? (unsigned)((tcr >> 38) & 1) : (unsigned)((tcr >> 37) & 1);
    unsigned epd  = sel ? (unsigned)((tcr >> 23) & 1) : (unsigned)((tcr >> 7) & 1);
    unsigned top  = tbi ? 55 : 63;
    u64 ttbr = sel ? c->ttbr1[1] : c->ttbr0[1];
    unsigned txsz = sel ? (unsigned)((tcr >> 16) & 0x3f) : (unsigned)(tcr & 0x3f);
    if (txsz < 16) txsz = 16;
    if (txsz > 39) txsz = 39;
    unsigned va_size = 64 - txsz;

    if (epd) return FSC_TRANS_L0;                          /* table walks disabled for this regime */

    /* Bits [top : va_size] must be a clean sign-extension of the region (all 0
     * for TTBR0, all 1 for TTBR1); otherwise the VA is out of range and takes a
     * level-0 translation fault instead of silently aliasing into the tables. */
    u64 chk = (~0ULL << va_size);
    if (top < 63) chk &= (1ULL << (top + 1)) - 1;         /* exclude the tag byte */
    if ((va & chk) != (sel ? chk : 0ULL)) return FSC_TRANS_L0;

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
            if (attridx) *attridx = (desc >> 2) & 7;
            if (sh) *sh = (desc >> 8) & 3;
            if (blk_shift) *blk_shift = shift;
            return 0;
        }
        if ((desc & 3) == 3) {
            if (level == 3) {                                      /* page */
                *pa_page = desc & 0x0000FFFFFFFFF000ULL;
                r_ap = (desc >> 6) & 3; r_pxn = (desc >> 53) & 1; r_uxn = (desc >> 54) & 1;
                if (((desc >> 10) & 1) == 0) return FSC_ACCESS_L0 + level;
                *ap = r_ap; *uxn = r_uxn; *pxn = r_pxn;
                if (attridx) *attridx = (desc >> 2) & 7;
                if (sh) *sh = (desc >> 8) & 3;
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
    /* An LDTR/STTR at EL1 matches and fills the EL0-tagged entries: same
     * EL1&0 regime, EL0 permission view, and the tag mismatch keeps its
     * (more restrictive) permissions from ever serving a normal EL1 access. */
    bool el0 = acc_el0(c);
    if (e->valid && e->gen == g_tlb_gen && e->va_page == page
        && e->el0 == el0
        && perm_ok_el(acc, e->ap, e->uxn, e->pxn, el0)) {
        *pa_out = e->pa_page | (va & 0xfff);
        return true;
    }

    /* Either a TLB miss, or a hit whose cached permissions deny this access.
     * In the latter case do NOT fault on the stale entry: arm64 software may
     * make a descriptor more permissive (set AF, set the dirty/write bits on a
     * COW or first-write fault) WITHOUT issuing a TLBI, relying on a hardware
     * re-walk when the cached restrictive entry causes a spurious permission
     * fault. So always re-walk from the table before deciding; only a fresh
     * walk that still denies the access is a real fault. Without this, the
     * kernel's in-place RO->RW upgrade (e.g. copy_to_user hitting a write-
     * protected COW page) is never observed and the access faults forever -> the
     * copy returns EFAULT ("Bad address"). */
    u64 pa_page; u8 ap, uxn, pxn;
    unsigned blk_shift = 0;
    unsigned fsc = walk(c, va, &pa_page, &ap, &uxn, &pxn, NULL, NULL, &blk_shift);
    if (fsc) { raise_abort(c, va, acc, fsc); return false; }
    if (blk_shift) lp_record(va, blk_shift);   /* caching a block fragment */

    e->valid = 1; e->gen = g_tlb_gen; e->va_page = page; e->pa_page = pa_page;
    e->ap = ap; e->uxn = uxn; e->pxn = pxn; e->el0 = el0;

    if (!perm_ok_el(acc, ap, uxn, pxn, el0)) { raise_abort(c, va, acc, FSC_PERM_L3); return false; }
    *pa_out = pa_page | (va & 0xfff);
    return true;
}

/* ---------- typed accesses ---------- */

/* Insert a D-TLB entry for a RAM page the slow path just accessed. Fill-on-
 * slow-path-only is load-bearing: a write that misses (or hits an entry
 * without W) always reaches mmu_translate, whose deny-triggered re-walk
 * observes in-place RO->RW descriptor upgrades (the COW contract above), and
 * the refill then grants W. Watchpoint runs keep every access on the slow
 * path so the AEWATCH/AEVAW hooks in mem_write/phys_write still see them. */
static void dtlb_fill(CPU *c, u64 va, u64 pa) {
    if (g_watch || g_vawatch) return;
    Machine *m = c->m;
    u64 pa_page = pa & ~0xfffULL;
    if (pa_page < m->ram_base || pa_page - m->ram_base >= m->ram_size) return;
    u64 r = DTLB_R, w = DTLB_W;
    if (c->sctlr[1] & 1) {
        TLBEntry *t = &tlb[tlb_idx(va)];   /* just filled/validated for va */
        if (t->va_page != (va & ~0xfffULL) || t->el0 != acc_el0(c)) return;
        r = perm_ok(c, ACC_READ,  t->ap, t->uxn, t->pxn) ? DTLB_R : 0;
        w = perm_ok(c, ACC_WRITE, t->ap, t->uxn, t->pxn) ? DTLB_W : 0;
    }
    if (g_jit_code_bitmap && w) {
        u64 pfn = (pa_page - m->ram_base) >> 12;
        if (g_jit_code_bitmap[pfn >> 3] & (1u << (pfn & 7))) w = 0;
    }
    DTlbEnt *de = dtlb_ent(va);
    de->tag = dtlb_tag(c, va);
    de->pte = (u64)(uintptr_t)(m->ram + (pa_page - m->ram_base)) | r | w;
}

bool mem_read(CPU *c, u64 va, unsigned size, u64 *out) {
    u64 off = va & 0xfffULL;
    if (off + size > 0x1000 && (c->sctlr[1] & 1)) {       /* spans two pages */
        unsigned first = 0x1000u - (unsigned)off;
        u64 lo = 0, hi = 0;
        if (!mem_read(c, va, first, &lo)) return false;
        if (!mem_read(c, va + first, size - first, &hi)) return false;
        *out = lo | (hi << (first * 8));
        return true;
    }
    /* off+size can exceed the page only with the MMU off (no split above);
     * keep that on the bus path — the last RAM page must not memcpy past the
     * host mapping, and a RAM->MMIO crossing is a single bus access there. */
    DTlbEnt *de = dtlb_ent(va);
    if (off + size <= 0x1000 && de->tag == dtlb_tag(c, va) && (de->pte & DTLB_R)) {
        u64 v = 0;
        __builtin_memcpy(&v, (const u8 *)(uintptr_t)(de->pte & ~0xfffULL) + off, size);
        *out = v;
        return true;
    }
    u64 pa;
    if (!mmu_translate(c, va, ACC_READ, &pa)) return false;
    u64 v = phys_read(c->m, pa, size);
    if (c->m->last_bus_status != BUS_OK) return raise_abort(c, va, ACC_READ, FSC_EXTERNAL);
    *out = v;
    dtlb_fill(c, va, pa);
    return true;
}

bool mem_write(CPU *c, u64 va, unsigned size, u64 val) {
    if (__builtin_expect(g_vawatch != 0, 0) &&    /* #16: single hook guard */
        va < g_vawatch + 8 && va + size > g_vawatch) {
        static int vn = 0;
        if (vn++ < 400) {
            fprintf(stderr, "[vaw] W va=0x%llx size=%u val=0x%llx pc=0x%llx el=%u icount=%llu\n",
                    (unsigned long long)va, size, (unsigned long long)val,
                    (unsigned long long)c->cur_insn_pc, c->el, (unsigned long long)c->icount);
            heaptrack_query(c, va);
            if (c->el == 0) ring_dump();   /* EL0 writes are the suspect ash overrun */
        }
    }
    u64 off = va & 0xfffULL;
    if (off + size > 0x1000 && (c->sctlr[1] & 1)) {       /* spans two pages */
        unsigned first = 0x1000u - (unsigned)off;
        if (!mem_write(c, va, first, val)) return false;
        if (!mem_write(c, va + first, size - first, val >> (first * 8))) return false;
        return true;
    }
    DTlbEnt *de = dtlb_ent(va);   /* no-cross guard: see mem_read */
    if (off + size <= 0x1000 && de->tag == dtlb_tag(c, va) && (de->pte & DTLB_W)) {
        __builtin_memcpy((u8 *)(uintptr_t)(de->pte & ~0xfffULL) + off, &val, size);
        return true;
    }
    u64 pa;
    if (!mmu_translate(c, va, ACC_WRITE, &pa)) return false;
    phys_write(c->m, pa, size, val);
    if (c->m->last_bus_status != BUS_OK) return raise_abort(c, va, ACC_WRITE, FSC_EXTERNAL);
    /* JIT self-modifying-code hook: the D-TLB refuses the W bit for pages
     * holding translated code, so stores to them always land here — drop
     * the page's translations now that the store has committed. */
    if (g_jit_code_bitmap) {
        u64 pa_page = pa & ~0xfffULL;
        Machine *m = c->m;
        if (pa_page >= m->ram_base && pa_page - m->ram_base < m->ram_size) {
            u64 pfn = (pa_page - m->ram_base) >> 12;
            if (g_jit_code_bitmap[pfn >> 3] & (1u << (pfn & 7)))
                jit_invalidate_phys_range(pa, size);
        }
    }
    dtlb_fill(c, va, pa);
    return true;
}

/* Non-faulting translate+read for diagnostics: returns false if the VA does not
 * translate (instead of raising an abort). Ignores permissions. */
bool mem_peek(CPU *c, u64 va, unsigned size, u64 *out) {
    if ((c->sctlr[1] & 1) == 0) { *out = phys_read(c->m, va, size);
        return c->m->last_bus_status == BUS_OK; }
    u64 pa_page; u8 ap, uxn, pxn;
    if (walk(c, va, &pa_page, &ap, &uxn, &pxn, NULL, NULL, NULL)) return false;
    *out = phys_read(c->m, pa_page | (va & 0xfff), size);
    return c->m->last_bus_status == BUS_OK;
}

bool mmu_probe_pa(CPU *c, u64 va, u64 *pa_out) {
    if ((c->sctlr[1] & 1) == 0) { *pa_out = va; return true; }
    TLBEntry *e = &tlb[tlb_idx(va)];
    if (e->valid && e->gen == g_tlb_gen && e->va_page == (va & ~0xfffULL)
        && e->el0 == (c->el == 0)) {
        *pa_out = e->pa_page | (va & 0xfff);
        return true;
    }
    u64 pa_page; u8 ap, uxn, pxn;
    if (walk(c, va, &pa_page, &ap, &uxn, &pxn, NULL, NULL, NULL)) return false;
    *pa_out = pa_page | (va & 0xfff);
    return true;
}

/* AT S1E1R/W + S1E0R/W: probe the stage-1 translation of va and return the
 * PAR_EL1 value. Uses walk() directly — no exception, no TLB fill (walk is
 * side-effect-free), and AF==0 duly reports the access-flag FST.
 * PAR_EL1 (F==0): PA[47:12], ATTR[63:56] = the MAIR byte the leaf's AttrIndx
 * selects, SH[8:7] from the descriptor, NS[9] = 1 (non-secure-only machine).
 * PAR_EL1 (F==1): FST[6:1], PTW=0, S=0 (stage 1), bit 11 RES1. */
u64 mmu_at_s1(CPU *c, u64 va, bool is_write, bool as_el0) {
    /* Stage 1 disabled: flat translation (attributes: ATTR/SH left 0). */
    if ((c->sctlr[1] & 1) == 0)
        return (va & 0x0000FFFFFFFFF000ULL) | (1ULL << 9);
    u64 pa_page; u8 ap, uxn, pxn, attridx = 0, sh = 0;
    unsigned fsc = walk(c, va, &pa_page, &ap, &uxn, &pxn, &attridx, &sh, NULL);
    if (!fsc && !perm_ok_el(is_write ? ACC_WRITE : ACC_READ, ap, uxn, pxn, as_el0))
        fsc = FSC_PERM_L3;                 /* same convention as mmu_translate */
    if (fsc)
        return 1ULL | ((u64)(fsc & 0x3f) << 1) | (1ULL << 11);
    u64 mair_byte = (c->mair[1] >> (attridx * 8)) & 0xff;
    return (pa_page & 0x0000FFFFFFFFF000ULL) | ((u64)sh << 7) | (1ULL << 9)
         | (mair_byte << 56);
}

/* Return a host pointer to the start of a guest physical page if it is backed
 * by RAM or flash, else NULL (MMIO / unmapped — must go through the bus). */
static u8 *host_page_ptr(Machine *m, u64 pa_page) {
    if (pa_page >= m->ram_base && pa_page + 0x1000 <= m->ram_base + m->ram_size)
        return m->ram + (pa_page - m->ram_base);
    if (pa_page >= m->flash_base && pa_page + 0x1000 <= m->flash_base + m->flash_size)
        return m->flash + (pa_page - m->flash_base);
    return NULL;
}

bool mem_ifetch_slow(CPU *c, u64 va, u32 *insn_out) {
    if (va & 3) {                       /* PC-alignment fault (EC 0x22), no ISS */
        cpu_raise_sync(c, esr_make(EC_PC_ALIGN, 0), va);
        return false;
    }
    u64 pa;
    if (!mmu_translate(c, va, ACC_EXEC, &pa)) return false;
    u8 *hp = host_page_ptr(c->m, pa & ~0xfffULL);
    if (hp) {
        /* Cache the page translation; bytes are still read live from *host. */
        FetchCache *f = &g_fcache[c->el == 0];
        f->host = hp;
        f->page = va & ~0xfffULL;
        f->pa_page = pa & ~0xfffULL;
        f->mmu  = (u8)(c->sctlr[1] & 1);
        __builtin_memcpy(insn_out, hp + (va & 0xfffULL), 4);
        return true;
    }
    /* Executing from MMIO/unmapped space: read through the bus, do not cache. */
    u64 v = phys_read(c->m, pa, 4);
    if (c->m->last_bus_status != BUS_OK) return raise_abort(c, va, ACC_EXEC, FSC_EXTERNAL);
    *insn_out = (u32)v;
    return true;
}

/* One D-TLB probe + one 16-byte host copy when the access stays inside a
 * page. The fallback two-halves path is observably identical for the cases
 * the fast path skips (page-crossing, MMIO, watchpoint runs — the D-TLB is
 * empty then — and JIT code pages, whose W bit the D-TLB refuses): both
 * orders write/read the same bytes and fault at the same split VA. */
bool mem_read128(CPU *c, u64 va, V128 *out) {
    u64 off = va & 0xfffULL;
    DTlbEnt *de = dtlb_ent(va);
    if (off <= 0xff0 && de->tag == dtlb_tag(c, va) && (de->pte & DTLB_R)) {
        __builtin_memcpy(out, (const u8 *)(uintptr_t)(de->pte & ~0xfffULL) + off, 16);
        return true;
    }
    u64 lo, hi;
    if (!mem_read(c, va, 8, &lo)) return false;
    if (!mem_read(c, va + 8, 8, &hi)) return false;
    out->d[0] = lo; out->d[1] = hi;
    return true;
}

bool mem_write128(CPU *c, u64 va, const V128 *val) {
    u64 off = va & 0xfffULL;
    DTlbEnt *de = dtlb_ent(va);
    if (off <= 0xff0 && de->tag == dtlb_tag(c, va) && (de->pte & DTLB_W)) {
        __builtin_memcpy((u8 *)(uintptr_t)(de->pte & ~0xfffULL) + off, val, 16);
        return true;
    }
    if (!mem_write(c, va, 8, val->d[0])) return false;
    if (!mem_write(c, va + 8, 8, val->d[1])) return false;
    return true;
}
