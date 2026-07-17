/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* JIT internals shared between the runtime (jit.c) and the host backends.
 * One backend compiles per host (self-selected by #ifdef in backend_*.c);
 * jit.c provides inert stubs on hosts without one, so the runtime links —
 * and stays ILP32-clean — everywhere. */
#ifndef A64_JIT_PRIV_H
#define A64_JIT_PRIV_H

#include <stddef.h>
#include "cpu.h"
#include "mmu.h"     /* D-TLB layout (probe ABI), fetch cache */
#include "jit.h"

#ifndef LIKELY
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
#define GUEST_PAGE_SIZE 4096ULL

/* Donor-compat alias: the backends' inline D-TLB probe indexes by entry
 * count. The probe emitters are adapted to mmu.h's DTlbEnt layout: tag =
 * VA page | env->dtlb_ctxgen (flush generation + MMU + EL0), pte = host
 * page | W(2) | R(1). Only the interpreter's slow path fills entries. */
#define A64_DTLB_ENTRIES DTLB_SIZE

/* Inline-exclusives emitter: compiled out (see backend_x86_64.c note). */
#define JIT_BE_ATOMICS 0

/* Basic-block budget: capped length, and a translation never crosses a 4 KB
 * guest page boundary (invalidation is page-granular). JIT_INSN_MAX_BYTES is
 * the worst-case emitted host bytes per guest instruction across backends;
 * translate() reserves the full budget up front so emission cannot overrun. */
#define JIT_MAX_BLOCK_INSNS 128
#define JIT_INSN_MAX_BYTES  64
#define JIT_BLOCK_MAX_BYTES (JIT_MAX_BLOCK_INSNS * JIT_INSN_MAX_BYTES + 256)

#define JIT_HASH_BITS 14                      /* block table buckets/thread */
#define JIT_HASH_SIZE (1u << JIT_HASH_BITS)
#define JIT_PAGE_BITS 12                      /* page->blocks index buckets */
#define JIT_PAGE_TBL  (1u << JIT_PAGE_BITS)
#define JIT_MAX_BLOCKS 65536                  /* arena cap; full -> flush */

/* Exit identifiers returned by a block run: (block index << 1) | slot for a
 * chainable exit that just went back to the dispatcher, EXIT_NONE otherwise.
 * jit_run patches the exit site to jump straight to the successor block. */
#define JIT_EXIT_NONE 0xffffffffu

typedef struct JBlock {
    u64 pc;                     /* guest entry address */
    u64 tag;                    /* pc | ctx bits 1:0 (EL0, MMU-on) — the
                                 * lookup key; a block is only valid for the
                                 * (EL, MMU-state) context it was built in */
    const u8 *host_page;        /* host pointer of the code page at translate
                                 * time; dispatch re-resolves pc's page and
                                 * requires a match, so a VA remap (TTBR
                                 * switch, new mapping) misses naturally */
    u64 pa_page;                /* physical page (invalidation/thrash key) */
    u32 ninsns;                 /* guest instructions covered */
    u32 in_head;                /* incoming chained-edge list (edge pool), ~0 end */
    const u8 *code;             /* entry point in the RX view */
    struct JBlock *hash_next;   /* pc-hash chain */
    struct JBlock *page_next;   /* per-guest-page chain (invalidation) */
    u64 exit_pc[2];             /* chainable successor pcs (~0 = none) */
    u32 exit_off[2];            /* patch-site offset from code, in bytes */
    u32 stub_word0[2];          /* original first word at the patch site
                                 * (unpatch restores it; arm64 backend) */
    u8  patched[2];
} JBlock;

/* Incoming chain edges (for unpatching when a block is invalidated). */
typedef struct JEdge {
    u32 from;                   /* block index */
    u8  slot;
    u32 next;                   /* ~0 end */
} JEdge;

/* Per-thread JIT state. Generated code pins a host register on this struct
 * (x86-64: r15, arm64: x27) and addresses the fields BEFORE jcache at fixed
 * small offsets (arm64 LDR imm12 reach); keep new generated-code-visible
 * fields in that leading group, 8-aligned. */
typedef struct JitEnv {
    CPU *c;                     /* pinned second register loads this (offset 0) */
    volatile u32 interrupt;     /* safepoint flag: signal/invalidate/mapping */
    u32 active;
    void *helper_exec1;         /* u32 (*)(CPU*, u64 pc, u32 insn) */
    void *helper_exec1_ic;      /* same, for IC IVAU (invalidates after) */
    void *dtlb;                 /* this thread's D-TLB base (jit_dtlb_base) */
    void *helper_ld;            /* u32 (*)(CPU*, u64 va, u64 pc, u32 desc) */
    void *helper_st;            /* u32 (*)(CPU*, u64 va, u64 val, u64 pc, u32) */
    void *helper_ldv;           /* u32 (*)(CPU*, u64 va, u64 pc, u32 desc) */
    void *helper_stv;           /* u32 (*)(CPU*, u64 va, u64 pc, u32 desc) */
    u64 tmp_spill[4];           /* spill homes for IR temps 0-2 (generated
                                 * code); slot 3 saves a fused memory run's
                                 * base VA across its bail-path helper calls */
    u64 vconst[2];              /* 128-bit constant staging slot: generated
                                 * code stores a translate-time constant here
                                 * and movdqu-loads it (x86 pshufb LUTs and
                                 * byte-shift masks; no RIP-relative pools) */
    u32 slowmem;                /* every mem op takes the helper path; wired
                                 * on until Stage 2b adapts the inline probe
                                 * (then AEJIT_SLOWMEM=1 forces, bisection) */
    u64 ctx;                    /* current 4-bit block ctx (EL0|MMU<<1|spx<<2);
                                 * jcache probes OR it into pc<<2 */
    u32 jc_gen;                 /* g_tlb_gen the jcache was last purged at */
    u64 dtlb_ctxgen;            /* low 12 bits of the current dtlb_tag():
                                 * (g_tlb_gen & 0x3ff) << 2 | MMU<<1 | EL0.
                                 * Inline probes OR this into the VA page for
                                 * the tag compare; refreshed by the
                                 * dispatcher every iteration (gen/EL/MMU can
                                 * only change via block-exiting paths) */
    u64 icount_deadline;        /* block-entry safepoint: exit when
                                 * c->icount >= this (bounds IRQ/tick latency
                                 * across chained blocks) */

    /* Indirect-branch target cache, probed inline by generated code for
     * BR/BLR/RET: guest pc -> block entry. Purged on any invalidation. */
#define JIT_JC_BITS 12
#define JIT_JC_SIZE (1u << JIT_JC_BITS)
    /* Indirect-branch inline cache: tag = block tag (pc<<2 | ctx); the
     * probes in generated code OR env->ctx into the target pc. Filled by
     * the dispatcher after a verified lookup; purged (to ALL-ONES, the
     * unhittable empty pattern — see jcache_purge) on flush, page drop and
     * TLB-generation change, so a hit can never outlive the mapping it was
     * verified under. */
    struct JCEnt { u64 tag; const u8 *code; } jcache[JIT_JC_SIZE];
    /* Dirty-index list so a purge clears only the entries actually filled
     * since the last purge (a TLBI-heavy phase purges constantly but fills
     * few entries in between; a full 64KB memset per TLBI costs as much as
     * the IC IVAU flush storm the jcache replaced). Saturation (> MAX
     * fills) falls back to the full memset. */
#define JIT_JC_DIRTY_MAX 1024
    u16 jc_dirty[JIT_JC_DIRTY_MAX];
    u32 jc_ndirty;

    const u8 *epilogue_rx;      /* generated blocks jump here to exit */
    u32 (*enter)(struct JitEnv *env, const u8 *code_rx);   /* returns exit id */

    /* Code cache. rw == rx unless the W^X fallback dual-mapped a memfd. */
    u8 *cache_rw, *cache_rx;
    u8 *ptr, *end;              /* bump cursor / limit, in the RW view */
    u8 *blocks_start_rw;        /* flush resets ptr here (thunks precede it) */
    size_t cache_size;
    int memfd;                  /* backing fd for the dual-map case, else -1 */

    JBlock **hash;              /* [JIT_HASH_SIZE] */
    JBlock **pages;             /* [JIT_PAGE_TBL], chained via page_next */
    JBlock *arena;
    u32 nblocks;
    JEdge *edges;               /* incoming-chain edge pool */
    u32 nedges;
    u32 flush_count;            /* invalidates the dispatcher's chain pointer */
    u32 pending_flush;          /* safety valve: a helper that must not flush
                                 * mid-block can set this; the dispatcher
                                 * flushes before the next dispatch. Nothing
                                 * sets it today (IC IVAU invalidates its one
                                 * page precisely). */
    u64 ntrans;                 /* lifetime translations (AEJIT_STATS) */
    u64 ndisp;                  /* lifetime dispatcher round trips */

    /* Self-modifying-code thrash guard: a direct-mapped count of how often
     * each guest page has been invalidated. A page rewritten in a tight loop
     * (each rewrite = IC IVAU = drop + retranslate) is run purely
     * interpreted instead, so retranslation storms can't dominate. */
#define JIT_THRASH_SLOTS 64
#define JIT_THRASH_LIMIT 32
    struct { u64 page; u32 count; } thrash[JIT_THRASH_SLOTS];
} JitEnv;

extern JitEnv g_jit_env;        /* single CPU, single thread: one global */

/* Emission cursor. rw is where bytes are written, rx the address the same
 * bytes will execute at; they advance in lockstep (branch displacements are
 * computed against rx). Overflow is latched, never trapped. */
typedef struct Emit {
    u8 *rw;
    const u8 *rx;
    u8 *rw_end;
    int overflow;
} Emit;

/* ---- backend surface (backend_x86_64.c / backend_a64.c) ---- */

struct IRBlock;

int  be_available(void);
/* Emit the enter/exit thunks once per cache; sets env->enter/epilogue_rx. */
void be_emit_thunks(Emit *e, JitEnv *env);
/* Emit a whole block (entry safepoint, body, exit stubs). Fills b->exit_*.
 * Returns 0, or -1 on emission-buffer overflow (caller retries smaller). */
int  be_emit_block(Emit *e, JitEnv *env, JBlock *b, const struct IRBlock *ir);
/* Rewrite chainable exit `slot` of b to jump straight to target_rx, and the
 * inverse (restore the dispatcher stub). Same-thread only. */
void be_patch_chain(JitEnv *env, JBlock *b, int slot, const u8 *target_rx);
void be_unpatch_chain(JitEnv *env, JBlock *b, int slot);
/* Can this host inline the given IRO_VOP class for this insn word?
 * (Per-host fidelity/capability table; 0 = keep the exec_fpsimd helper.) */
int  be_vop_ok(unsigned vclass, u32 insn);
/* Make [rx, rx+len) (written via rw) visible to instruction fetch. */
void be_flush_icache(const u8 *rx, const u8 *rw, size_t len);

/* ---- helpers called from generated code (jit.c) ---- */

u32 jit_exec1(CPU *c, u64 pc, u32 insn);
u32 jit_exec1_ic(CPU *c, u64 pc, u32 insn);

/* Memory slow paths. desc packs the access shape (see jit.c). Each sets
 * cur_insn_pc = pc for a precise fault and returns 1 if the access faulted
 * (block must exit), 0 on success (result already committed to CPU state). */
u32 jit_ld(CPU *c, u64 va, u64 pc, u32 desc);
u32 jit_st(CPU *c, u64 va, u64 val, u64 pc, u32 desc);
u32 jit_ldv(CPU *c, u64 va, u64 pc, u32 desc);   /* into c->v[rt] */
u32 jit_stv(CPU *c, u64 va, u64 pc, u32 desc);   /* from c->v[rt] */

/* Memory-access descriptor bit layout (shared by frontend, backends, jit.c). */
#define MDESC_RT(d)    ((d) & 31)
#define MDESC_SZLOG(d) (((d) >> 5) & 3)          /* 0=1B,1=2B,2=4B,3=8B */
#define MDESC_SIGN(d)  (((d) >> 7) & 1)          /* sign-extend (loads) */
#define MDESC_IS64(d)  (((d) >> 8) & 1)          /* result width */
#define MDESC_VSZL(d)  (((d) >> 9) & 7)          /* vector byte-log: 0..4 = 1..16B */
#define MDESC_TMP(d)   (((d) >> 12) & 1)         /* loads: RT field is an IR temp
                                                  * index; commit to tmp_spill[RT]
                                                  * (LDP's all-or-nothing halves) */
#define MDESC_MAKE(rt, szlog, sign, is64) \
    ((u32)((rt) | ((szlog) << 5) | ((sign) << 7) | ((is64) << 8)))
#define MDESC_MAKEV(rt, vszlog) ((u32)((rt) | ((vszlog) << 9)))
#define MDESC_TMPBIT   (1u << 12)

#endif /* A64_JIT_PRIV_H */
