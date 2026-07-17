/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Virtual memory translation + typed guest memory access.
 * M0/M1: flat (MMU off). M2 extends mmu_translate for SCTLR.M=1. */
#ifndef A64_MMU_H
#define A64_MMU_H

#include "cpu.h"

typedef enum { ACC_READ, ACC_WRITE, ACC_EXEC } AccType;

/* Translate VA->PA for the current EL/state. On fault, raises the appropriate
 * synchronous exception (data/instruction abort) and returns false. */
bool mmu_translate(CPU *c, u64 va, AccType acc, u64 *pa_out);

/* Typed accesses. Return false if a fault was raised (caller must abort the
 * instruction and let the exception take effect). On success, *out holds data. */
bool mem_read(CPU *c, u64 va, unsigned size, u64 *out);
bool mem_write(CPU *c, u64 va, unsigned size, u64 val);

/* Non-faulting translate+read for diagnostics (false if VA doesn't translate). */
bool mem_peek(CPU *c, u64 va, unsigned size, u64 *out);

/* Instruction-fetch fast path. Caches the host base pointer of the current code
 * page so sequential fetches skip the TLB hash + bus dispatch. Only the page
 * *translation* is cached (a host base pointer), never decoded bytes — the
 * instruction word is read live, so decompressed/self-modifying code in the page
 * is reflected immediately. Cleared by tlb_flush_all (the same invalidation
 * contract the software TLB already relies on; the tag also carries EL0/MMU
 * state so EL or MMU-enable changes fall through to the slow path). */
typedef struct {
    u64  page;   /* VA page base of the cached translation */
    u8  *host;   /* host pointer to that guest page (RAM/flash); NULL = invalid */
    u8   el0;    /* EL0 flag of the cached translation */
    u8   mmu;    /* SCTLR_EL1.M of the cached translation */
} FetchCache;
extern FetchCache g_fcache;

/* Slow path: translate, refresh the fetch cache, read. Used on a cache miss. */
bool mem_ifetch_slow(CPU *c, u64 va, u32 *insn_out);

static inline bool mem_ifetch(CPU *c, u64 va, u32 *insn_out) {
    u64 page = va & ~0xfffULL;
    if (g_fcache.host && g_fcache.page == page &&
        g_fcache.el0 == (u8)(c->el == 0) &&
        g_fcache.mmu == (u8)(c->sctlr[1] & 1)) {
        u32 v;
        __builtin_memcpy(&v, g_fcache.host + (va & 0xfffULL), 4);
        *insn_out = v;
        return true;
    }
    return mem_ifetch_slow(c, va, insn_out);
}

/* Block helpers for SIMD 128-bit and pair accesses (two 64-bit halves). */
bool mem_read128(CPU *c, u64 va, V128 *out);
bool mem_write128(CPU *c, u64 va, const V128 *val);

/* Invalidate the software TLB (called on TLBI / TTBR / context changes). */
void tlb_flush_all(void);

/* ---------------- data-access fast path: host-pointer D-TLB ----------------
 * Direct-mapped cache of VA page -> host RAM page, probed by mem_read/mem_write
 * before the full translate+bus path. RAM pages only: flash (CFI command state)
 * and MMIO (side effects) are never inserted, so they always take the bus path.
 * The entry layout is an ABI shared with the JIT backends (which emit this
 * probe inline) — do not change it without updating them.
 *
 * tag: VA page | flush-generation (bits 11:2) | SCTLR.M (bit 1) | EL0 (bit 0).
 *      Folding the generation into the tag makes tlb_flush_all O(1): bumping
 *      g_tlb_gen invalidates every entry without touching the array. The
 *      10-bit slice aliases every 1024 flushes; tlb_flush_all memsets then.
 * pte: 4 KB-aligned host page pointer | write-allowed (bit 1) | read (bit 0).
 *      A zeroed entry has no permission bits, so it can never fast-path. */
#define DTLB_BITS 10
#define DTLB_SIZE (1u << DTLB_BITS)
#define DTLB_R    1u
#define DTLB_W    2u
typedef struct { u64 tag; u64 pte; } DTlbEnt;
_Static_assert(sizeof(DTlbEnt) == 16, "DTlbEnt layout is a JIT backend ABI");
extern DTlbEnt g_dtlb[DTLB_SIZE];
extern u32 g_tlb_gen;

/* Pages holding JIT-translated code: the D-TLB refuses the W bit for them so
 * guest stores fall to the slow path, where the JIT invalidates the page's
 * blocks (self-modifying-code coherence). NULL until a JIT allocates it. */
extern const u8 *g_jit_code_bitmap;

static inline u64 dtlb_tag(const CPU *c, u64 va) {
    return (va & ~0xfffULL) | ((u64)(g_tlb_gen & 0x3ff) << 2)
         | ((c->sctlr[1] & 1) ? 2u : 0u) | (c->el == 0 ? 1u : 0u);
}
static inline DTlbEnt *dtlb_ent(u64 va) {
    return &g_dtlb[(va >> 12) & (DTLB_SIZE - 1)];
}

#endif /* A64_MMU_H */
