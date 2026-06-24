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
bool mem_ifetch(CPU *c, u64 va, u32 *insn_out);

/* Block helpers for SIMD 128-bit and pair accesses (two 64-bit halves). */
bool mem_read128(CPU *c, u64 va, V128 *out);
bool mem_write128(CPU *c, u64 va, const V128 *val);

/* Invalidate the software TLB (called on TLBI / TTBR / context changes). */
void tlb_flush_all(void);

#endif /* A64_MMU_H */
