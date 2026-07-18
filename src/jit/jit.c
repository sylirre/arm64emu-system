/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* JIT runtime: code cache, block tables, the dispatch loop, and full-system
 * coherence. Ported from arm64chroot and collapsed to one global, lock-free
 * runtime (single CPU, single thread).
 *
 * Translation unit is a basic block (ends at any branch/system instruction,
 * capped, never crossing a 4 KB guest page). Anything not translated inline
 * is a call to jit_exec1 -> exec_a64, so semantics are the interpreter's by
 * construction.
 *
 * Full-system coherence (deltas vs the donor):
 *  - Block identity: lookup key is (pc | EL0/MMU ctx) AND the code page's
 *    host pointer as resolved at dispatch time through the fetch cache. A
 *    TTBR switch or remap changes the resolution and misses naturally —
 *    correctness never depends on observing TLBI. Chaining is restricted to
 *    same-page, same-ctx successors so a chained jump can never cross a
 *    translation boundary unverified.
 *  - Self-modifying code: pages holding translations are marked in
 *    g_jit_code_bitmap; the D-TLB refuses the W bit for them (mmu.c), so
 *    every store to such a page reaches mem_write's slow path, which calls
 *    jit_invalidate_phys_range before... after committing the store. A store
 *    into the *currently running* block's page drops the block from the
 *    tables (safe: code memory is bump-allocated, never reused until a full
 *    flush) and the stale tail keeps running to the block's end —
 *    architecturally permitted without IC IVAU+ISB, and bounded at
 *    JIT_MAX_BLOCK_INSNS. Cross-page SMC (module loads, kernel alternatives,
 *    DMA) is exact. IC IVAU drops the (usually already-dropped) target page's
 *    blocks precisely — never a full flush; a boot issues hundreds of
 *    thousands of them. pending_flush stays as a dispatcher-side safety
 *    valve (a full flush must never run mid-block).
 *  - IRQ/timer: the block-entry safepoint exits when c->icount reaches
 *    env->icount_deadline, so chained hot loops return to the run loop at
 *    machine_tick cadence; IRQ lines are only raised by machine_tick /
 *    synchronous MMIO, and the dispatcher re-checks them between blocks.
 *  - D-TLB: helper-path accesses go through mem_read/mem_write, whose probe
 *    tags carry g_tlb_gen — TLBI correctness is inherited from the
 *    interpreter, with no separate JIT resync protocol. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "machine.h"
#include "esr.h"
#include "predecode.h"
#include "jit_priv.h"
#include "ir.h"

int g_jit;                          /* -jit (main.c) */
JitEnv g_jit_env;

/* RAM code-page bitmap (1 bit per 4 KB page). mmu.c reads it through the
 * const alias g_jit_code_bitmap (declared in mmu.h) when filling D-TLB write
 * entries; jit.c owns the writable pointer. NULL until the JIT starts. */
static u8 *g_code_bitmap;
static u64 g_code_bitmap_bits;

/* ---- backend stubs for hosts without a code generator ---- */
#if !defined(__x86_64__) && !defined(__aarch64__)
int  be_available(void) { return 0; }
void be_emit_thunks(Emit *e, JitEnv *env) { (void)e; (void)env; }
int  be_emit_block(Emit *e, JitEnv *env, JBlock *b, const struct IRBlock *ir) {
    (void)e; (void)env; (void)b; (void)ir;
    return -1;
}
void be_patch_chain(JitEnv *env, JBlock *b, int slot, const u8 *target_rx) {
    (void)env; (void)b; (void)slot; (void)target_rx;
}
void be_unpatch_chain(JitEnv *env, JBlock *b, int slot) {
    (void)env; (void)b; (void)slot;
}
void be_flush_icache(const u8 *rx, const u8 *rw, size_t len) {
    (void)rx; (void)rw; (void)len;
}
int be_vop_ok(unsigned vclass, u32 insn) {
    (void)vclass; (void)insn;
    return 0;
}
#endif

int jit_backend_available(void) { return be_available(); }

void jit_request_exit(void) {
    g_jit_env.interrupt = 1;        /* single store: async-signal-safe */
}

/* ---- optional helper-call profiler (AEJIT_STATS=1 or =/path) ----
 * Ranks the exact instruction words still executed through the exec_a64
 * helper, so inlining work can be aimed at what a workload actually runs. */
#define JSTAT_SLOTS 4096
typedef struct { u32 word; u32 pad_; u64 count; } JStat;
static int g_jit_stats = -1;                 /* -1 until first jit_env_init */
static int g_jstat_fd = -1;                  /* parked dup of stderr */
static const char *g_jstat_path;             /* AEJIT_STATS=/file: append */
static JStat g_jstat[JSTAT_SLOTS];
static u64 g_jstat_lost;

static void jstat_bump(u32 insn) {
    u32 h = (insn ^ (insn >> 13) ^ (insn >> 25)) & (JSTAT_SLOTS - 1);
    for (u32 i = 0; i < 8; i++) {
        JStat *s = &g_jstat[(h + i) & (JSTAT_SLOTS - 1)];
        if (s->count == 0) s->word = insn;
        if (s->word == insn) { s->count++; return; }
    }
    g_jstat_lost++;
}

static const char *jstat_class(u32 w) {
    if ((w >> 24) == 0xD5) return "system";
    if ((w & 0x3F000000u) == 0x08000000u) return "excl";
    if ((w & 0x3B200C00u) == 0x38200000u) return "lse";
    unsigned grp = (w >> 25) & 0xf;
    if (grp == 0xa || grp == 0xb) return "branch";
    if ((grp & 5) == 4) return "ldst";
    if ((grp & 7) == 7) return "fpsimd";
    return "other";
}

static int jstat_cmp(const void *a, const void *b) {
    u64 ca = ((const JStat *)a)->count, cb = ((const JStat *)b)->count;
    return (ca < cb) - (ca > cb);
}

static void jstat_dump(void) {
    static int done;
    if (done) return;
    done = 1;
    u64 total = g_jstat_lost;
    for (u32 i = 0; i < JSTAT_SLOTS; i++) total += g_jstat[i].count;
    int fd = g_jstat_path
                 ? open(g_jstat_path, O_WRONLY | O_CREAT | O_APPEND, 0644)
                 : g_jstat_fd;
    if (total && fd >= 0) {
        u64 icount = g_jit_env.active ? g_jit_env.c->icount : 0;
        qsort(g_jstat, JSTAT_SLOTS, sizeof *g_jstat, jstat_cmp);
        dprintf(fd, "[jit-stats] %llu helper insns",
                (unsigned long long)total);
        if (icount)
            dprintf(fd, " / %llu executed (%.2f%%)",
                    (unsigned long long)icount,
                    100.0 * (double)total / (double)icount);
        dprintf(fd, "\n");
        dprintf(fd, "[jit-stats] %u flushes, %llu translations, %llu dispatches\n",
                (unsigned)g_jit_env.flush_count,
                (unsigned long long)g_jit_env.ntrans,
                (unsigned long long)g_jit_env.ndisp);
        for (u32 i = 0; i < 32 && g_jstat[i].count; i++)
            dprintf(fd, "[jit-stats]  %12llu  %08x  %s\n",
                    (unsigned long long)g_jstat[i].count,
                    (unsigned)g_jstat[i].word, jstat_class(g_jstat[i].word));
        if (g_jstat_lost)
            dprintf(fd, "[jit-stats]  %12llu  (table overflow)\n",
                    (unsigned long long)g_jstat_lost);
    }
    if (g_jstat_path && fd >= 0) close(fd);
}

void jit_stats_flush(void) {
    if (g_jit_stats > 0) jstat_dump();
}

/* ---- optional per-block code dump (AEJIT_DUMP=<prefix> or =1) ----
 * Sparse image of the code cache + one .map line per block; disassemble with
 *   objdump -D -b binary -m i386:x86-64 --start-address=0x<off> ... .code */
static const char *g_jdump_prefix;           /* NULL = off */
static int g_jdump_code_fd = -1, g_jdump_map_fd = -1;

static void jdump_open(JitEnv *env) {
    char name[256];
    snprintf(name, sizeof name, "%s.%d.code", g_jdump_prefix, (int)getpid());
    g_jdump_code_fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    snprintf(name, sizeof name, "%s.%d.map", g_jdump_prefix, (int)getpid());
    g_jdump_map_fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_jdump_code_fd >= 0)                /* thunks live at offset 0 */
        (void)!pwrite(g_jdump_code_fd, env->cache_rw,
                      (size_t)(env->blocks_start_rw - env->cache_rw), 0);
}

static void jdump_block(JitEnv *env, CPU *c, JBlock *b, const u8 *rw_end) {
    if (g_jdump_code_fd < 0 && g_jdump_map_fd < 0) jdump_open(env);
    size_t off = (size_t)(b->code - env->cache_rx);
    size_t len = (size_t)(rw_end - (env->cache_rw + off));
    if (g_jdump_code_fd >= 0)
        (void)!pwrite(g_jdump_code_fd, env->cache_rw + off, len, (off_t)off);
    if (g_jdump_map_fd < 0) return;
    char line[JIT_MAX_BLOCK_INSNS * 9 + 96];
    int k = snprintf(line, sizeof line,
                     "pc=0x%llx off=0x%zx len=%zu ninsns=%u guest:",
                     (unsigned long long)b->pc, off, len, b->ninsns);
    for (u32 i = 0; i < b->ninsns && k < (int)sizeof line - 10; i++) {
        u32 w;
        if (!mem_ifetch(c, b->pc + 4 * i, &w)) break;
        k += snprintf(line + k, sizeof line - (size_t)k, " %08x", w);
    }
    line[k++] = '\n';
    (void)!write(g_jdump_map_fd, line, (size_t)k);
}

/* ---- RAM code-page bitmap ---- */

static inline int bitmap_pfn(Machine *m, u64 pa_page, u64 *pfn) {
    if (pa_page < m->ram_base || pa_page - m->ram_base >= m->ram_size)
        return 0;
    *pfn = (pa_page - m->ram_base) >> 12;
    return 1;
}

static void bitmap_set(Machine *m, u64 pa_page) {
    u64 pfn;
    if (g_code_bitmap && bitmap_pfn(m, pa_page, &pfn))
        g_code_bitmap[pfn >> 3] |= (u8)(1u << (pfn & 7));
}

static void bitmap_clear(Machine *m, u64 pa_page) {
    u64 pfn;
    if (g_code_bitmap && bitmap_pfn(m, pa_page, &pfn))
        g_code_bitmap[pfn >> 3] &= (u8)~(1u << (pfn & 7));
}

static int bitmap_test(Machine *m, u64 pa_page) {
    u64 pfn;
    if (g_code_bitmap && bitmap_pfn(m, pa_page, &pfn))
        return (g_code_bitmap[pfn >> 3] >> (pfn & 7)) & 1;
    return 0;
}

/* ---- code cache ---- */

static size_t jit_cache_size(void) {
    const char *s = getenv("AEJIT_MB");
    long mb = s ? atol(s) : 32;
    if (mb < 1) mb = 1;
    if (mb > 128) mb = 128;   /* AArch64 B imm26 (+-128 MiB) must span the cache */
    return (size_t)mb << 20;
}

/* RWX if the host allows it; else a dual-mapped memfd (RW + RX views of the
 * same pages) for W^X hosts; else the JIT degrades to the interpreter. */
static int cache_alloc(JitEnv *env) {
    size_t size = jit_cache_size();
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        env->cache_rw = env->cache_rx = p;
        env->memfd = -1;
        env->cache_size = size;
        return 0;
    }
#ifdef SYS_memfd_create
    int fd = (int)syscall(SYS_memfd_create, "arm64emu-jit", 1 /*MFD_CLOEXEC*/);
    if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
        void *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        void *rx = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        if (rw != MAP_FAILED && rx != MAP_FAILED) {
            env->cache_rw = rw;
            env->cache_rx = rx;
            env->memfd = fd;
            env->cache_size = size;
            return 0;
        }
        if (rw != MAP_FAILED) munmap(rw, size);
        if (rx != MAP_FAILED) munmap(rx, size);
    }
    if (fd >= 0) close(fd);
#endif
    return -1;
}

static const u8 *rx_of(JitEnv *env, const u8 *rw) {
    return env->cache_rx + (rw - env->cache_rw);
}

/* Empty the indirect-branch cache. The empty pattern is all-ones, not zero:
 * a probe key is (pc<<2)|ctx, whose bit 3 is always clear (pc is 4-aligned
 * and this machine's ctx fits in 3 bits), so an all-ones tag can never be
 * hit — while a zeroed tag would match a `br` to VA 0 at EL1/MMU-off and
 * jump generated code to a NULL entry pointer. */
static void jcache_purge(JitEnv *env) {
    if (env->jc_ndirty <= JIT_JC_DIRTY_MAX) {
        for (u32 i = 0; i < env->jc_ndirty; i++)
            env->jcache[env->jc_dirty[i]].tag = ~0ULL;
    } else {
        memset(env->jcache, 0xff, sizeof env->jcache);
    }
    env->jc_ndirty = 0;
}

/* Must only run from the dispatcher's C loop, never from a helper while a
 * block is still on the native call stack (its memory would be reused). */
static void jit_flush_all(JitEnv *env) {
    memset(env->hash, 0, JIT_HASH_SIZE * sizeof *env->hash);
    memset(env->pages, 0, JIT_PAGE_TBL * sizeof *env->pages);
    jcache_purge(env);
    env->nblocks = 0;
    env->nedges = 0;
    env->ptr = env->blocks_start_rw;
    env->flush_count++;
}

static void jit_env_destroy(JitEnv *env) {
    if (env->cache_rw) munmap(env->cache_rw, env->cache_size);
    if (env->cache_rx && env->cache_rx != env->cache_rw)
        munmap((void *)env->cache_rx, env->cache_size);
    if (env->memfd >= 0) close(env->memfd);
    free(env->hash);
    free(env->pages);
    free(env->arena);
    free(env->edges);
    memset(env, 0, sizeof *env);
    env->memfd = -1;
}

static int jit_env_init(JitEnv *env, CPU *c) {
    if (!be_available()) return -1;
    memset(env, 0, sizeof *env);
    env->c = c;
    env->memfd = -1;
    env->helper_exec1 = (void *)jit_exec1;
    env->helper_exec1_ic = (void *)jit_exec1_ic;
    env->helper_ld = (void *)jit_ld;
    env->helper_st = (void *)jit_st;
    env->helper_ldv = (void *)jit_ldv;
    env->helper_stv = (void *)jit_stv;
    env->helper_spchk = (void *)jit_sp_check;
    env->dtlb = g_dtlb;
    /* Full memset, not jcache_purge: the dirty list is empty here but the
     * zeroed entries must still become the all-ones empty pattern. */
    memset(env->jcache, 0xff, sizeof env->jcache);
    env->jc_gen = g_tlb_gen;
    env->slowmem = getenv("AEJIT_SLOWMEM") != NULL;
    if (g_jit_stats < 0) {
        const char *s = getenv("AEJIT_STATS");
        g_jit_stats = s != NULL;
        if (g_jit_stats) {
            if (s[0] == '/') g_jstat_path = s;
            else {
                g_jstat_fd = fcntl(2, F_DUPFD_CLOEXEC, 900);
                if (g_jstat_fd < 0) g_jstat_fd = 2;
            }
            atexit(jstat_dump);
        }
        const char *d = getenv("AEJIT_DUMP");
        if (d) g_jdump_prefix = (d[0] && strcmp(d, "1") != 0)
                                    ? d : "aejit-dump";
    }
    if (cache_alloc(env) < 0) return -1;
    env->hash = calloc(JIT_HASH_SIZE, sizeof *env->hash);
    env->pages = calloc(JIT_PAGE_TBL, sizeof *env->pages);
    env->arena = malloc(JIT_MAX_BLOCKS * sizeof *env->arena);
    env->edges = malloc(2 * JIT_MAX_BLOCKS * sizeof *env->edges);
    g_code_bitmap_bits = c->m->ram_size >> 12;
    g_code_bitmap = calloc(1, (g_code_bitmap_bits >> 3) + 1);
    if (!env->hash || !env->pages || !env->arena || !env->edges ||
        !g_code_bitmap) {
        free(g_code_bitmap);
        g_code_bitmap = NULL;
        jit_env_destroy(env);
        return -1;
    }
    g_jit_code_bitmap = g_code_bitmap;       /* mmu.c starts honoring it */
    Emit e = { env->cache_rw, env->cache_rx,
               env->cache_rw + 4096, 0 };
    be_emit_thunks(&e, env);
    if (e.overflow) { jit_env_destroy(env); return -1; }
    be_flush_icache(env->cache_rx, env->cache_rw,
                    (size_t)(e.rw - env->cache_rw));
    env->blocks_start_rw =
        env->cache_rw + (((e.rw - env->cache_rw) + 15) & ~15L);
    env->ptr = env->blocks_start_rw;
    env->end = env->cache_rw + env->cache_size;
    env->active = 1;
    return 0;
}

/* ---- block table ---- */

static inline u32 hash_tag(u64 tag) { return (u32)(tag >> 4) & (JIT_HASH_SIZE - 1); }

static JBlock *jit_lookup(JitEnv *env, u64 tag, const u8 *host_page) {
    JBlock *b = env->hash[hash_tag(tag)];
    while (b && (b->tag != tag || b->host_page != host_page))
        b = b->hash_next;
    return b;
}

/* ---- SMC thrash guard (keyed by physical page) ---- */
static void thrash_bump(JitEnv *env, u64 pa_page) {
    unsigned s = (unsigned)(pa_page >> 12) & (JIT_THRASH_SLOTS - 1);
    if (env->thrash[s].page != pa_page) {
        env->thrash[s].page = pa_page;
        env->thrash[s].count = 1;
    } else if (env->thrash[s].count < 0xffffffu) {
        env->thrash[s].count++;
    }
}
static int thrash_hot(JitEnv *env, u64 pa_page) {
    unsigned s = (unsigned)(pa_page >> 12) & (JIT_THRASH_SLOTS - 1);
    return env->thrash[s].page == pa_page &&
           env->thrash[s].count >= JIT_THRASH_LIMIT;
}

/* Remove b from the hash table and unpatch every chained direct jump INTO it
 * back to its dispatcher stub. The caller removes b from its page-list. b's
 * code memory stays allocated, so a block invalidating itself from a store
 * helper can still run its own tail. */
static void jit_unlink_block(JitEnv *env, JBlock *b) {
    JBlock **hp = &env->hash[hash_tag(b->tag)];
    while (*hp && *hp != b) hp = &(*hp)->hash_next;
    if (*hp) *hp = b->hash_next;
    for (u32 ei = b->in_head; ei != ~0u; ) {
        JEdge *ed = &env->edges[ei];
        JBlock *from = &env->arena[ed->from];
        if (from->patched[ed->slot]) {
            be_unpatch_chain(env, from, ed->slot);
            be_flush_icache(from->code + from->exit_off[ed->slot],
                            env->cache_rw + (from->code - env->cache_rx) +
                                from->exit_off[ed->slot], 16);
        }
        ei = ed->next;
    }
    b->in_head = ~0u;
}

/* Drop translations whose code lies on physical page pa_page. */
static void jit_drop_page(JitEnv *env, u64 pa_page) {
    JBlock **pp = &env->pages[(u32)(pa_page >> 12) & (JIT_PAGE_TBL - 1)];
    int dropped = 0;
    while (*pp) {
        JBlock *b = *pp;
        if (b->pa_page == pa_page) {
            *pp = b->page_next;
            jit_unlink_block(env, b);
            dropped = 1;
        } else {
            pp = &b->page_next;
        }
    }
    /* jcache entries may point into the dropped blocks; unlike chain edges
     * they carry no unpatch list, so purge wholesale (page drops are rare —
     * real SMC only). */
    if (dropped) jcache_purge(env);
}

/* ---- helpers called from generated code ---- */

/* Execute one instruction with interpreter semantics; mirrors cpu_step's
 * ordering exactly (icount++ runs even when exec_a64 raised an abort).
 * Returns nonzero when the block must stop: control transfer (including a
 * synchronous exception vectoring), stop, or WFI/WFE halt. */
u32 jit_exec1(CPU *c, u64 pc, u32 insn) {
    c->cur_insn_pc = pc;
    c->pc = pc + 4;
    exec_a64(c, insn);
    c->icount++;
    if (UNLIKELY(g_jit_stats > 0)) jstat_bump(insn);
    if (UNLIKELY(c->stop || c->halted)) return 1;
    return c->pc != pc + 4;
}

/* ---- memory slow paths (see jit_priv.h MDESC_*) ----
 * On a fault the exception has already been taken synchronously (c->pc is
 * the vector); count the faulting instruction here — it is excluded from the
 * block's batched icount, and the interpreter counts aborted instructions
 * (cpu_step runs icount++ unconditionally after exec_a64). */

/* SP-alignment check (SCTLR_EL1.SA/SA0). Generated code calls this only when the
 * SP base is already known-misaligned (low 4 bits set). Raises an SP-alignment
 * fault and returns 1 (exit the block) when the check is enabled for the current
 * EL; returns 0 when it is off (the native access then proceeds). Counts the
 * faulting instruction like the mem helpers above. */
u32 jit_sp_check(CPU *c, u64 pc) {
    c->cur_insn_pc = pc;
    unsigned bit = (c->el == 0) ? 4 : 3;      /* SCTLR_EL1.SA0 : .SA */
    if ((c->sctlr[1] >> bit) & 1) {
        cpu_raise_sync(c, esr_make(EC_SP_ALIGN, 0), 0);
        c->icount++;
        return 1;
    }
    return 0;
}

u32 jit_ld(CPU *c, u64 va, u64 pc, u32 desc) {
    c->cur_insn_pc = pc;
    unsigned rt = MDESC_RT(desc), sz = 1u << MDESC_SZLOG(desc);
    u64 v;
    if (!mem_read(c, va, sz, &v)) { c->icount++; return 1; }
    if (MDESC_SIGN(desc)) {
        unsigned b = sz * 8;
        v = (u64)((s64)(v << (64 - b)) >> (64 - b));
    }
    if (!MDESC_IS64(desc)) v = (u32)v;
    if (MDESC_TMP(desc)) g_jit_env.tmp_spill[rt & 3] = v;   /* IR temp home */
    else if (rt != 31) c->x[rt] = v;
    return 0;
}

u32 jit_st(CPU *c, u64 va, u64 val, u64 pc, u32 desc) {
    c->cur_insn_pc = pc;
    if (!mem_write(c, va, 1u << MDESC_SZLOG(desc), val)) {
        c->icount++;
        return 1;
    }
    return 0;
}

u32 jit_ldv(CPU *c, u64 va, u64 pc, u32 desc) {
    c->cur_insn_pc = pc;
    unsigned rt = MDESC_RT(desc), bytes = 1u << MDESC_VSZL(desc);
    if (bytes == 16) {
        V128 v;
        if (!mem_read128(c, va, &v)) { c->icount++; return 1; }
        c->v[rt] = v;
    } else {
        u64 t;
        if (!mem_read(c, va, bytes, &t)) { c->icount++; return 1; }
        c->v[rt].d[0] = t;
        c->v[rt].d[1] = 0;
    }
    return 0;
}

u32 jit_stv(CPU *c, u64 va, u64 pc, u32 desc) {
    c->cur_insn_pc = pc;
    unsigned rt = MDESC_RT(desc), bytes = 1u << MDESC_VSZL(desc);
    if (bytes == 16) {
        if (!mem_write128(c, va, &c->v[rt])) { c->icount++; return 1; }
        return 0;
    }
    if (!mem_write(c, va, bytes, c->v[rt].d[0])) { c->icount++; return 1; }
    return 0;
}

/* IC IVAU, Xt: with store-tracking the real invalidation already happened at
 * the store (mem_write slow path, DMA and flash hooks), so translations for
 * the line's page are near-always already gone and the code bitmap is clear.
 * Belt-and-braces: resolve Xt without faulting (the insn is a no-op in
 * sysreg.c and never faults — stay bug-compatible) and drop that one page's
 * surviving blocks. jit_invalidate_phys_range is bitmap-gated and in-helper-
 * safe (dropped block memory is never reused before a dispatcher-run
 * flush_all), so this must never escalate to a full flush — a boot issues
 * hundreds of thousands of IC IVAUs (one per line of every executable page
 * it faults in). Ends the block: the page may be the running block's own. */
u32 jit_exec1_ic(CPU *c, u64 pc, u32 insn) {
    jit_exec1(c, pc, insn);
    u64 pa;
    if (mmu_probe_pa(c, reg_x(c, insn & 0x1f), &pa))
        jit_invalidate_phys_range(pa, 1);
    return 1;
}

/* ---- translation ---- */

static IRBlock *g_ir;               /* ~17 KB: heap-allocated lazily */

static JBlock *jit_translate(JitEnv *env, CPU *c, u64 pc, u64 tag,
                             const u8 *host_page, u64 pa_page) {
    if (!g_ir) {
        g_ir = malloc(sizeof *g_ir);
        if (!g_ir) { g_jit = 0; return NULL; }
    }
    u32 max_insns = JIT_MAX_BLOCK_INSNS;
retry:
    if (env->nblocks >= JIT_MAX_BLOCKS ||
        env->nedges + 2 > 2 * JIT_MAX_BLOCKS ||
        (size_t)(env->end - env->ptr) < JIT_BLOCK_MAX_BYTES)
        jit_flush_all(env);

    u32 n = jit_fe_block(c, pc, g_ir, max_insns);
    if (n == 0) return NULL;        /* entry fetch fault already taken */

    Emit e = { env->ptr, rx_of(env, env->ptr),
               env->ptr + JIT_BLOCK_MAX_BYTES, 0 };
    JBlock *b = &env->arena[env->nblocks];
    b->pc = pc;
    b->tag = tag;
    b->host_page = host_page;
    b->pa_page = pa_page;
    b->ninsns = n;
    b->code = e.rx;

    if (be_emit_block(&e, env, b, g_ir) < 0) {
        if (max_insns > 1 && n > 1) {   /* pathological block: shrink, retry */
            max_insns = n / 2;
            goto retry;
        }
        fprintf(stderr, "arm64emu: JIT emitter overflow, disabling jit\n");
        g_jit = 0;
        return NULL;
    }

    env->nblocks++;
    b->hash_next = env->hash[hash_tag(tag)];
    env->hash[hash_tag(tag)] = b;
    JBlock **ph = &env->pages[(u32)(pa_page >> 12) & (JIT_PAGE_TBL - 1)];
    b->page_next = *ph;
    *ph = b;

    /* First translation on this physical page: mark it and strip the W bit
     * from any D-TLB entry already pointing at it, so future stores take the
     * slow path and hit the invalidation hook. (The host page pointer
     * identifies the physical page across all VA aliases.) */
    if (!bitmap_test(c->m, pa_page)) {
        bitmap_set(c->m, pa_page);
        for (u32 i = 0; i < DTLB_SIZE; i++)
            if ((g_dtlb[i].pte & ~0xfffULL) == (u64)(uintptr_t)host_page)
                g_dtlb[i].pte &= ~(u64)DTLB_W;
    }

    be_flush_icache(b->code, env->ptr, (size_t)(e.rw - env->ptr));
    if (UNLIKELY(g_jdump_prefix != NULL)) jdump_block(env, c, b, e.rw);
    env->ptr = env->cache_rw + (((e.rw - env->cache_rw) + 15) & ~15L);
    env->ntrans++;
    return b;
}

/* ---- coherence entry points ---- */

void jit_invalidate_phys_range(u64 pa, u64 len) {
    if (!g_jit || !len) return;
    JitEnv *env = &g_jit_env;
    if (!env->active) return;
    Machine *m = env->c->m;
    u64 first = pa & ~(GUEST_PAGE_SIZE - 1);
    u64 npages = ((pa + len - 1 - first) >> 12) + 1;
    for (u64 i = 0; i < npages; i++) {
        u64 pg = first + (i << 12);
        /* RAM pages are bitmap-guarded; non-RAM (flash) pages are rare
         * writes — walk their (near-always empty) page chain directly. */
        u64 pfn;
        if (bitmap_pfn(m, pg, &pfn) && !bitmap_test(m, pg)) continue;
        jit_drop_page(env, pg);
        thrash_bump(env, pg);
        bitmap_clear(m, pg);
    }
}

/* PSCI warm reboot: drop everything (RAM persists but the OS is going away;
 * the fresh kernel would miss on ctx/host-page anyway — this reclaims the
 * cache and resets the thrash guard). Runs from main.c between steps, never
 * mid-block. */
void jit_reset(void) {
    JitEnv *env = &g_jit_env;
    if (!env->active) return;
    jit_flush_all(env);
    memset(env->thrash, 0, sizeof env->thrash);
    if (g_code_bitmap)
        memset(g_code_bitmap, 0, (size_t)(g_code_bitmap_bits >> 3) + 1);
    env->pending_flush = 0;
}

/* ---- dispatch ---- */

/* Block context: EL0 flag, MMU enable, the active SP bank (sp_el[] is
 * banked by SPSel/EL — the backends bake the bank's offset into generated
 * code, donor's user-mode assumption of sp_el[0] does not hold here), and
 * the CPACR.FPEN trap state (bit 4: the frontend compiles FP/SIMD as a
 * trapping CALL1 under it, see fe_fptrap). All of these can only change via
 * CALL1-ended blocks (exceptions, ERET, MSR SPSel/CPACR), so they are
 * per-block constants. */
static inline u64 jit_ctx(const CPU *c) {
    unsigned spx = c->sp_sel ? c->el : 0;
    return (u64)((c->el == 0 ? 1u : 0u) | ((c->sctlr[1] & 1) ? 2u : 0u) |
                 ((u64)spx << 2) | (c->fp_trapped ? 16u : 0u));
}

static inline u64 jit_tag(u64 pc, u64 ctx) {
    /* pc bits 1:0 are zero -> 5 ctx bits fit under pc << 3; the 3 dropped
     * pc top bits are redundant sign-extension for canonical VAs. */
    return (pc << 3) | ctx;
}

/* Fetch-cache-based resolution of pc's code page. Returns 1 with host/pa
 * set, 0 if the fetch faulted (exception taken) or the page is MMIO
 * (host = NULL). Mirrors mem_ifetch's tag discipline. */
static int resolve_code_page(CPU *c, u64 pc, const u8 **host, u64 *pa_page) {
    u64 page = pc & ~0xfffULL;
    if (!(g_fcache.host && g_fcache.page == page &&
          g_fcache.el0 == (u8)(c->el == 0) &&
          g_fcache.mmu == (u8)(c->sctlr[1] & 1))) {
        u32 w0;
        c->cur_insn_pc = pc;             /* precise IABORT ELR (cpu_step:447) */
        if (!mem_ifetch_slow(c, pc, &w0)) return 0;   /* abort taken */
        if (!(g_fcache.host && g_fcache.page == page &&
              g_fcache.el0 == (u8)(c->el == 0) &&
              g_fcache.mmu == (u8)(c->sctlr[1] & 1))) {
            *host = NULL;                /* executing from MMIO */
            return 1;
        }
    }
    *host = g_fcache.host;
    *pa_page = g_fcache.pa_page;
    return 1;
}

StepResult jit_step(CPU *c, u64 slice, u64 max_insn) {
    JitEnv *env = &g_jit_env;
    if (UNLIKELY(!env->active)) {
        if (jit_env_init(env, c) < 0) {
            fprintf(stderr, "arm64emu: cannot allocate JIT code cache, "
                            "using interpreter\n");
            g_jit = 0;
            return cpu_step(c);
        }
    }

    /* cpu_step's preamble, kept in lockstep with cpu.c. */
    if (c->stop) return STEP_HALT;
    if (c->fiq_line && !(c->daif & PS_F)) {
        exception_take(c, EXC_FIQ, 0, 0, c->pc);
        return STEP_OK;
    }
    if (c->irq_line && !(c->daif & PS_I)) {
        exception_take(c, EXC_IRQ, 0, 0, c->pc);
        return STEP_OK;
    }
    if (c->halted) return STEP_OK;

    /* Interpret-tail: near -maxinsn, single-step so the stop is exact. */
    if (max_insn && max_insn - c->icount < 2 * JIT_MAX_BLOCK_INSNS)
        return cpu_step(c);

    u64 deadline = c->icount + slice;
    /* Blocks entered below the deadline retire up to a full block past it:
     * pull the deadline in so -maxinsn is never crossed natively (the
     * interpret-tail above then single-steps to the exact boundary). */
    if (max_insn && deadline > max_insn - JIT_MAX_BLOCK_INSNS)
        deadline = max_insn - JIT_MAX_BLOCK_INSNS;
    env->icount_deadline = deadline;
    u32 flush_seen = env->flush_count;
    JBlock *prev = NULL;                /* chainable exit awaiting a target */
    int prev_slot = 0;

    while (LIKELY(!c->stop && !c->halted && c->icount < deadline)) {
        /* IRQ lines can be raised mid-slice by guest MMIO writes (GIC/timer
         * are updated synchronously): let the next jit_step deliver. */
        if (UNLIKELY((c->irq_line && !(c->daif & PS_I)) ||
                     (c->fiq_line && !(c->daif & PS_F))))
            break;
        if (UNLIKELY(env->interrupt)) { env->interrupt = 0; break; }
        if (UNLIKELY(env->pending_flush)) {
            env->pending_flush = 0;
            jit_flush_all(env);
        }
        if (UNLIKELY(env->flush_count != flush_seen)) {
            flush_seen = env->flush_count;
            prev = NULL;                /* arena reset: pointer is stale */
        }
        if (UNLIKELY(env->jc_gen != g_tlb_gen)) {
            /* TLBI: VA->code bindings may be stale. Every gen-changing path
             * ends its block, so the purge always runs before the next
             * generated-code jcache probe can hit. */
            env->jc_gen = g_tlb_gen;
            jcache_purge(env);
        }

        u64 pc = c->pc;
        env->ndisp++;
        const u8 *hp;
        u64 pa_page = 0;
        if (UNLIKELY(!resolve_code_page(c, pc, &hp, &pa_page))) {
            prev = NULL;                /* fetch abort taken: resume at vector */
            continue;
        }
        if (UNLIKELY(!hp)) {            /* MMIO execution: interpret */
            prev = NULL;
            cpu_step(c);
            continue;
        }
        if (UNLIKELY(thrash_hot(env, pa_page))) {
            /* Self-modifying hot page: interpret in place until control
             * leaves the page, so a rewrite loop can't thrash the
             * translator. cpu_step re-fetches live, exactly like the
             * interpreter. */
            prev = NULL;
            do {
                cpu_step(c);
            } while (!c->stop && !c->halted && c->icount < deadline &&
                     (c->pc & ~0xfffULL) == (pc & ~0xfffULL));
            continue;
        }

        u64 ctx = jit_ctx(c);
        env->ctx = ctx;                 /* jcache probes OR this into pc<<3 */
        /* Inline D-TLB probes OR these low tag bits into the VA page. Gen,
         * EL and MMU state can only change through block-exiting paths, so
         * refreshing here keeps every probe's compare current. */
        env->dtlb_ctxgen = ((u64)(g_tlb_gen & 0x3ff) << 2) | (ctx & 3);
        u64 tag = jit_tag(pc, ctx);
        JBlock *b = jit_lookup(env, tag, hp);
        if (!b) {
            b = jit_translate(env, c, pc, tag, hp, pa_page);
            if (UNLIKELY(env->flush_count != flush_seen)) {
                flush_seen = env->flush_count;
                prev = NULL;
            }
            if (!b) {
                if (!g_jit) return cpu_step(c);  /* JIT disabled itself */
                prev = NULL;            /* mid-block fetch fault: vector */
                continue;
            }
        }

        /* Chain the exit that brought us here directly to b — but only for a
         * same-page, same-ctx successor: the chained jump bypasses the
         * dispatcher's translation/context re-verification, and within one
         * page + one context the entry check that validated b covers prev's
         * page too. prev ran THIS iteration round, so no guest state changed
         * in between; skip if prev was invalidated meanwhile. */
        if (prev && prev->exit_pc[prev_slot] == b->pc &&
            !prev->patched[prev_slot] &&
            ((prev->tag ^ b->tag) & 0x1f) == 0 &&    /* all 5 ctx bits */
            ((prev->pc ^ b->pc) & ~0xfffULL) == 0 &&
            jit_lookup(env, prev->tag, prev->host_page) == prev &&
            env->nedges < 2 * JIT_MAX_BLOCKS) {
            be_patch_chain(env, prev, prev_slot, b->code);
            be_flush_icache(prev->code + prev->exit_off[prev_slot],
                            env->cache_rw + (prev->code - env->cache_rx) +
                                prev->exit_off[prev_slot], 16);
            JEdge *ed = &env->edges[env->nedges];
            ed->from = (u32)(prev - env->arena);
            ed->slot = (u8)prev_slot;
            ed->next = b->in_head;
            b->in_head = env->nedges++;
        }

        /* Refill the indirect-branch cache: b was just verified (tag +
         * host page) for this dispatch, so a generated JMPIND probe may
         * take the same tag straight to b->code until the next TLBI/
         * flush/page-drop purge. The entry key equals the block tag —
         * pc<<3 | ctx — never zero-extended pc alone (a `br` to VA 0
         * must not hit the empty pattern; see jcache_purge). */
        u32 jci = (u32)(pc >> 2) & (JIT_JC_SIZE - 1);
        env->jcache[jci].tag = tag;
        env->jcache[jci].code = b->code;
        if (env->jc_ndirty < JIT_JC_DIRTY_MAX)
            env->jc_dirty[env->jc_ndirty] = (u16)jci;
        if (env->jc_ndirty <= JIT_JC_DIRTY_MAX)   /* saturate, don't wrap */
            env->jc_ndirty++;

        u32 eid = env->enter(env, b->code);
        prev = NULL;
        if (eid != JIT_EXIT_NONE && (eid >> 1) < env->nblocks) {
            prev = &env->arena[eid >> 1];
            prev_slot = (int)(eid & 1);
        }
    }
    return c->stop ? STEP_HALT : STEP_OK;
}
