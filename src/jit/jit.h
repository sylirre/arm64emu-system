/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Optional JIT (-jit, off by default): translates guest basic blocks to host
 * native code. Interpreter semantics stay the source of truth: anything the
 * translator does not handle natively is executed by calling exec_a64, and
 * the JIT must keep the consistency suite (interpreter-vs-jit) green.
 *
 * Ported from arm64chroot and collapsed to a single global runtime: this
 * emulator is one CPU on one thread, so the donor's per-thread code caches,
 * thread registry, and cross-thread interrupt atomics are gone. Full-system
 * deltas vs the donor: blocks are keyed by (VA | EL0/MMU context) and
 * verified against the code page's host pointer at dispatch; self-modifying
 * code is caught by store-tracking (g_jit_code_bitmap keeps writes to
 * translated pages off the D-TLB fast path; the slow path invalidates), not
 * by IC IVAU interception; the block-entry safepoint bounds IRQ latency via
 * an icount deadline instead of a signal flag. */
#ifndef A64_JIT_H
#define A64_JIT_H

#include "cpu.h"

/* -jit CLI flag (parsed in main.c, defined in jit.c). Cleared at startup when
 * no backend exists for this host or a per-instruction debug facility is on,
 * and at runtime if the code cache cannot be allocated (W^X denial). */
extern int g_jit;

/* True if this build carries a code generator for the host architecture
 * (x86-64 or AArch64; other hosts run the interpreter). */
int jit_backend_available(void);

/* Run translated code for up to `slice` retired instructions (the caller's
 * machine_tick cadence), honoring cpu_step's preamble contract (stop, IRQ/FIQ
 * delivery, WFI). When max_insn is near, falls back to cpu_step so -maxinsn
 * stops at the exact instruction. Returns STEP_HALT iff the machine stopped. */
StepResult jit_step(CPU *c, u64 slice, u64 max_insn);

/* ---- Coherence hooks (all cheap no-ops when -jit is off) ---- */

/* A guest store or device DMA hit physical RAM holding translated code:
 * drop the affected pages' blocks. pa is a physical address. */
void jit_invalidate_phys_range(u64 pa, u64 len);

/* PSCI warm reboot (machine_reset): drop all translations and reset state. */
void jit_reset(void);

/* Called from the host signal catcher (async-signal-safe: one store): make
 * generated code exit at the next block entry. */
void jit_request_exit(void);

/* Flush the AEJIT_STATS report if enabled. No-op otherwise. */
void jit_stats_flush(void);

#endif /* A64_JIT_H */
