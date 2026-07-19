# The JIT (`--jit`)

Opt-in, off by default. Translates guest basic blocks to host native code
(x86-64 first-class; the AArch64 backend passes the full suite and
byte-identical consistency checkpoints under qemu-user emulation —
`make test-jit-a64` — but has not run on real ARM hardware).
The interpreter remains the default execution mode and the reference
semantics: anything the translator does not handle inline executes by
calling `exec_a64`, and `make test-jit` requires interpreter-vs-JIT
byte-identical CPU state on a deterministic-clock boot (`AE_RTCLOCK=0`, which
the gate pins; the runtime default is now the host wall clock).

Measured on the firmware+Linux boot used by `tests/run_jit_consist.sh`:
~36 MIPS interpreted, ~370 MIPS with `--jit` (≈10×). Steady state runs
>99.9% of instructions natively; the remaining helper residue
(~0.03%) is IC IVAU, TLBI and cold sysreg moves.

Ported from the arm64chroot user-mode emulator (same author; its CPU core
is a copy of this repo's). Its `docs/jit.md` describes the pipeline in
depth — predecode classifier (`src/jit/predecode.c`) → linear IR
(`src/jit/ir.h`, built by `src/jit/frontend.c`) → single-pass backend with
per-block greedy register allocation and lazy NZCV in host flags
(`src/jit/backend_*.c`) — plus block chaining, the bump-allocated code
cache, and the W^X fallback (RWX mmap → dual-mapped memfd → interpreter).
This document covers what is *different* here.

## Full-system deltas vs the donor

Single CPU, single thread: the donor's per-thread caches, thread registry
and cross-thread interrupt protocol collapsed into one global `JitEnv`,
no locks anywhere.

**Block identity.** A translation is keyed by
`(pc << 2) | ctx` where ctx = EL0 flag, `SCTLR_EL1.M`, and the active SP
bank (`sp_el[]` is banked by SPSel/EL; backends bake the bank's offset
into generated code — bugs here look like a firmware abort loop at the
vectors). In addition, the dispatcher re-resolves pc's *host code page*
through the fetch cache every dispatch and requires it to match the
block's; a TTBR switch or remap therefore misses naturally, and
correctness never depends on observing TLBI (the same "translate live"
philosophy the interpreter's fetch path uses). Exit chaining is
restricted to same-page, same-ctx successors so a chained jump can never
cross a translation boundary unverified.

Indirect branches (`RET`/`BR`/`BLR`) probe a small inline cache keyed by
the full block tag (the dispatcher refills it only after a verified
lookup). A hit skips the per-dispatch host-page re-verification, so the
cache is purged on every flush, page drop and TLB-generation change —
an entry can never outlive the mapping it was verified under, and a VA
remap *without* TLBI is outside the contract exactly as it is for the
interpreter's VA-tagged fetch cache. The empty pattern is all-ones
(a key's bit 3 is always clear), never zero: a zeroed entry would let a
`br` to VA 0 jump generated code to a NULL pointer.

**Memory accesses.** Generated loads/stores probe the interpreter's own
host-pointer D-TLB (`mmu.h: DTlbEnt`, a 16-byte-entry ABI): tag compare =
the access's last-byte page address OR `env->dtlb_ctxgen` (flush
generation + MMU + EL0) — page-crossers and stale generations mismatch by
construction, so a guest TLBI invalidates inline probes with no flush
protocol at all. Misses call `jit_ld/st/ldv/stv` → `mem_read/mem_write`
(the full translate + bus path; MMIO and flash never enter the D-TLB).
Entries are only ever filled by the interpreter's slow path. Faults are
taken synchronously by `cpu_raise_sync` inside the helper; the helper
returns nonzero, the block exits with precise state (base writeback is
emitted after the access, donor discipline), and the helper counts the
faulting instruction so `icount` matches `cpu_step`'s "execute then
count" order exactly.

**Self-modifying code.** The interpreter gets SMC for free (it re-reads
words live); the JIT uses store-tracking: pages holding translations are
marked in a physical-page bitmap (`g_jit_code_bitmap`), the D-TLB refuses
the W bit for them, so every store to such a page takes the slow path,
which drops the page's blocks after committing (`jit_invalidate_phys_range`).
Device DMA (`phys_write_blk`: virtio, fw_cfg, warm-reboot reloads) and
flash CFI programming invalidate the same way. `IC IVAU` resolves its
target line's page (non-faulting — the instruction is a no-op in
sysreg.c and never faults) and drops that one page's surviving blocks;
since store-tracking already invalidated at the store, this is
belt-and-braces and near-always a bitmap-gated no-op. It must never
escalate to a full flush: a boot issues one `IC IVAU` per cache line of
every executable page it faults in (~278k in the reference boot), and
full-flushing on each cost ~43% of total runtime before this was made
precise. A page rewritten in a tight loop trips the thrash guard and
runs interpreted until control leaves it.

**Interrupts and time.** `jit_step` replicates `cpu_step`'s preamble
(stop → FIQ/IRQ vs DAIF → WFI) exactly, then dispatches blocks until an
icount deadline one tick-slice ahead. The block-entry safepoint in
generated code checks `env->interrupt` and the deadline, so chained hot
loops return to the run loop at `machine_tick` cadence; IRQ lines raised
mid-slice by synchronous MMIO are re-checked between blocks. `--max-insn`
is exact: the deadline is capped a block short of the limit and the last
stretch single-steps through `cpu_step`.

**System instructions.** Hints, DMB/DSB (no-ops in an in-order
interpreter), `DC ZVA` (eight probed zero stores, mirroring sysreg.c),
and the hot moves that cannot change translation state are inline:
`TPIDR_EL0`/`TPIDRRO_EL0`/`TPIDR_EL1`, `SP_EL0` (only in blocks whose
context says the live SP is a different bank — otherwise sp_el[0] may be
cached in the backend's SP register), `DAIF` reads, `DAIFSet` (masking;
under AEDBG it stays on the helper so sysreg.c's break-on-mask hook
fires), and `DCZID_EL0` (constant). `MSR DAIF`/`DAIFClr` are inline but
still end the block: they may unmask, and the dispatcher must see
pending IRQ lines at the same boundary the helper path gave. Every other `0xD5` system
instruction — sysreg moves, TLBI, cache ops — runs through `exec_a64`
and *ends the block*, because it may change translation state; ISB ends
the block by definition (context synchronization = re-dispatch). SVC/
HVC/BRK/ERET run through `exec_a64` and end the block; the exception has
already vectored when the helper returns.

**Exclusives.** The donor's value-CAS emitters were discarded (its CPU
has `excl_val`; this emulator's monitor is address-match) and replaced
with a direct lowering of `decode.c ldst_exclusive`, quirks included:
the monitor compares address only (no size check), a faulting `ST*XR`
leaves the monitor set with Rs unwritten, and LDXP commits its
registers all-or-nothing. Loads are ordinary probed IR ops plus monitor
writes; stores bracket ordinary probed stores between a monitor-compare
(branch to fail) and a status-write/monitor-clear, so the D-TLB probe,
SMC write-refusal and fault-exit machinery are reused unchanged.
LDAR/STLR translate as plain accesses (exactly what the in-order
interpreter does); the `o2=1,o1=1` CAS space stays on `exec_a64` so
decode.c remains authoritative for it.

## Known, documented deviations from the interpreter

- **Tick cadence.** The interpreter polls devices every 1024 run-loop
  iterations; the JIT at block/slice boundaries. Both modes are
  individually deterministic, but deep in a boot the guest can observe
  timer values at slightly different icounts — visible as shifted printk
  timestamps. `tests/run_jit_consist.sh`'s checkpoints (through 300M
  instructions, past many timer IRQs) are byte-identical; a divergence
  there is a real bug, not this deviation.
- **Same-page self-modification.** A store into the *currently running*
  block's page invalidates the page's translations but lets the stale
  tail run to the block end (≤128 instructions) — architecturally
  permitted without IC IVAU+ISB, and unobservable for cross-page SMC
  (module loads, kernel alternatives, DMA), which is exact.
- **Debug facilities force the interpreter**: `--trace`, `--reg-trace`, AEPROF,
  AERING, AETPC, AECOV, AEWATCH, AEVAW (per-instruction hooks; the
  watchpoints also need every access on the slow path). `--jit` prints a
  notice and clears itself.

## Knobs (debug/bisection, all off by default)

- `AEJIT_MB=N` — code cache size (default 32, max 128).
- `AEJIT_STATS=1|/path` — rank instruction words still executed via the
  exec_a64 helper (what to inline next), plus a summary line of lifetime
  flushes / translations / dispatcher round trips.
- `AEJIT_DUMP=prefix` — sparse code-cache image + block map for objdump.
- `AEJIT_PDMAX=N` — translate only PD ops ≤ N natively (bisect a codegen
  bug to a handler class; branches stay native).
- `AEJIT_SLOWMEM=1` — force every memory access through the helpers.
- `AEJIT_NOFUSE=1` / `AEJIT_NOVRA=1` / `AEJIT_NOFP16=1` — disable memory-
  run fusing / the V-register cache / FP16 inlining.

## Verification

`make test-jit` runs the m1–m12 suite under `--jit` (the SIMD/crypto
batteries were built as QEMU differentials, so they double as the FP
oracle) and `tests/run_jit_consist.sh`: the deterministic firmware+Linux
boot (the harness pins `AE_RTCLOCK=0`) stopped at 1M/4M/16M/64M/300M
instructions, requiring byte-identical
serial output *and* final CPU state between modes. Divergence playbook:
binary-search `--max-insn` for the first divergent state (both modes are
deterministic; beware hangs — use `timeout`), then `AEJIT_PDMAX` to
isolate the instruction class, `AEJIT_SLOWMEM/NOFUSE/NOVRA` to isolate
machinery, `AEJIT_DUMP` + objdump to read the block. The full gates also
include `net_smoke.sh` under `EMU_FLAGS=--jit` and an Alpine ISO boot to
the login prompt.

One gate is mandatory for any frontend change: the **full 1.6B-insn boot
log compared against the interpreter's** — scripted as
`tests/run_bootlog_gate.sh` (`make test-jit-full`; also takes `--pd`). It
strips the `[ ts ]` printk prefixes and the clock-derived audit stamps;
anything still differing fails the gate. The consistency
checkpoints all sit inside the UEFI phase, and a translation bug in an
instruction UEFI doesn't lean on sails straight past them — a mistyped
sysreg encoding (CNTVCT misread as DCZID, freezing the guest's clock)
once passed 5/5 checkpoints and was caught only by this comparison.

`make test-jit-a64` cross-builds the AArch64-backend binary (clang +
lld + the Debian cross libc) and runs the suite plus a consistency
checkpoint under qemu-user — the standing guard that keeps the second
backend executable, not merely compilable.

`make fuzz-engines` runs the cross-engine differential fuzzer over the
inlined surface (random blocks, interpreter vs `--pd` vs `--jit` and its
SLOWMEM/NOFUSE/NOVRA variants). See `docs/parity.md` for the full parity
audit, verification matrix, and the bugs this fuzzer has caught.
