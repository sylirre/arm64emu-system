# arm64emu — Optimization Opportunities (TODO)

Survey of the interpreter/device hot paths begun 2026-07-03; **cleaned
2026-07-18** to drop what has landed and reframe the rest around the tiered
executor that now exists. Item numbers are preserved. Line references drift as
the tree moves — treat them as hints.

**Perf landscape:** the emulator now has three execution tiers — the plain
interpreter (portable reference), **`-pd`** (computed-goto direct-threaded, ~2.46×,
~90 MIPS), and **`-jit`** (native codegen, ~10×, ~370 MIPS). `-jit`/`-pd` bypass
the interpreter's per-instruction loop entirely — **for raw speed, use
`-jit`.** Everything the survey found has now landed or been measured and
rejected (see below); only the #14 build-flag pass and the #17 re-profile
remain open.

**Measure before/after every item.** Callgrind
(`valgrind --tool=callgrind ./arm64emu -bios … -maxinsn 300000000`), `AEPROF=1`
hot-PC histogram, and the deterministic full-boot wall-clock benchmark; plus
in-guest `dd if=/dev/zero of=/dev/null bs=1M` (DC ZVA path) and
`openssl speed sha256` (SIMD). Deterministic timer mode makes identical
`-maxinsn` runs directly comparable.

---

## Landed since the survey (do not re-do)

- **#1 — O(1) TLB flush via generation counter** (`mmu.c` `tlb_flush_all` =
  `++g_tlb_gen`; entries valid iff `gen == g_tlb_gen`). The ~14%-of-boot memset
  is gone.
- **#2 — host-pointer data TLB** (`mmu.c` `DTlbEnt g_dtlb` + the `mem_read`/
  `mem_write` fast path: index → tag compare → prot-bit test → fixed-offset
  host load/store; MMIO/flash fall through). The single biggest interpreter
  speedup, and it subsumes the hot-path of #3, #11, #12, #16 (see notes below).
- **Basic-block predecode** — the "viable evolution" the *Tried and rejected*
  section pointed at — shipped as the **`-pd`** and **`-jit`** tiers, which also
  supersede **#5** (batched run loop). See `docs/pd.md`, `docs/jit.md`.
- **#6 — split tick cadence** (2026-07-19): `machine_tick` keeps the timer/RTC
  compares tick-fine but runs the syscall-bound host-IO polls (UART/console
  `read(2)`, net `poll(2)`) every 65536 insns (`AEIOTICK` overrides;
  `machine_wait_for_event` forces a poll so WFI-idle latency is unchanged).
- **#7 — timer deadline compare** (2026-07-19): `timer_update` caches the next
  fire count (phys domain, `ARMTimer.deadline`); the per-tick `timer_check` is
  one compare. Every CNT* write already funnels through `timer_update`
  (`sysreg.c` CRn==14 hook), so the cache cannot go stale.
- **#8 — GIC early-out** (2026-07-19): `gic_set_irq` returns before `gic_update`
  when `pending` didn't move (the raw line isn't a `gic_best` input), killing
  the 2×/tick 256-entry rescan. Boot deltas for the batch: interp −4.4%,
  `-pd` −6.4%, `-jit` −16% (220→262 MIPS).
- **#4 stage 1 — TLBI by-VA** (2026-07-19): the EL1 VA forms (`VAE1`/`VAAE1`/
  `VALE1`/`VAALE1`[`IS`]) now take `tlb_flush_page`: clear the page's TLB +
  D-TLB slots by index and the fetch cache, leave `g_tlb_gen` alone. Guard: a
  VA inside the recorded union range of block-mapped fills (`lp_base/lp_mask`,
  QEMU-style — the TLB fragments 2 MB blocks into 4 KB entries) escalates to a
  full flush. The JIT purges its VA-keyed jcache on the new `g_tlbi_va_seq`
  (blocks are PA-keyed and re-verified at dispatch; TLBI CALL1s end their
  block, so the dispatcher-checked purge is race-free). Instrumented full boot:
  26773/28742 flush events (93%) became single-page; leftovers = 1645
  large-page escalations + 65 non-VA TLBIs + 259 TTBR/TCR/SCTLR flushes.
  Boot wall-clock neutral (±1%, interleaved A/B); fork/exec-heavy in-guest
  shell segment ≈2% faster under `-jit` — the win scales with user-space
  memory churn, which the boot barely exercises.
- **#11 — DC ZVA host memset** (2026-07-19): one D-TLB probe (64-aligned, never
  page-crossing) + `memset(host, 0, 64)`; miss/deny keeps the 8×`mem_write`
  loop (faults, MMIO, watchpoints, JIT code pages). Interp/`-pd` only — the
  JIT already inlines ZVA.
- **#12 — single-probe 16-byte accesses** (2026-07-19): `mem_read128`/
  `mem_write128` do one D-TLB probe + one 16-byte host copy when in-page
  (all Q loads/stores and Q/D pairs funnel through them), and `ldst_pair`
  routes X-pairs through the same helpers. Fallback = the old two-halves
  path, observably identical (same bytes, same split-VA fault). In-guest
  `-pd`: 2 GB `dd /dev/zero → /dev/null` −4.2%, 128 MB pipe copy −3.6%;
  boot neutral-to-better.
- **#18 — CRC32 slicing-by-8** (2026-07-19): `crc32_step` uses lazily-built
  8×256 tables per polynomial; CRC32X is one combined 8-lookup step (13.7×
  the bitwise helper). Benefits **all three tiers** — predecode leaves the
  CRC32 family GENERIC, so `-pd`/`-jit` call the same helper. Verified
  bit-exact vs the old code (KATs + 2M random inputs, both polys, all
  widths). No boot delta expected or seen: initramfs boots touch no
  CRC32C-checksummed filesystem; the win applies to ext4/btrfs guests.

- **#9/#10/#13/#15/#3/#16 — interpreter micro batch** (2026-07-19):
  add_with_carry skips the NZCV math when S==0 (#9); decode_bitmasks is
  memoized behind a 13-bit immN:immr:imms table, 32-bit = truncated 64-bit
  result (#10); the fetch cache is two entries indexed by `el == 0` so the
  syscall/ERET ping-pong keeps both code pages hot (#13, also felt by `-pd`
  which fetches through mem_ifetch); loads_stores tests register/pair forms
  first and `undefined` is cold (#15 — jump-table conversion not pursued:
  exec_a64's top level is already a switch and the chains are now
  frequency-ordered); phys_read/phys_write use fixed-size copies for the
  power-of-two sizes (#3); mem_write's vawatch check sits behind one
  `unlikely` guard (#16). Boot deltas: interp −4.6%, `-pd` −3.9%,
  `-jit` −1.4%.

---

## Measured and deferred

### 4 stage 2. ASID-tagged TLB entries
Stage 1 (TLBI by-VA, above) landed 2026-07-19. Stage 2 — tag entries with
ASID + nG and stop flushing on TTBR0 writes — was *measured and deferred*: an
instrumented full boot shows only **259** TTBR/TCR/SCTLR-write flushes total
(vs 26773 VA-TLBIs that stage 1 already made single-page), so its ceiling here
is a few hundred avoided O(1) flush+refill cycles per boot. It would also
change the D-TLB tag ABI shared with both JIT backends' inline probes
(`dtlb_ctxgen`) — high blast radius for negligible measured payoff. Revisit
only if a context-switch-heavy workload (many concurrent processes) profiles
TTBR0-write refills as hot.

---


## Build / micro

- **14. Build flags** (`Makefile`, still `-O2`, no LTO): measure `-O3`; `-flto`
  (the per-instruction path crosses main/cpu/decode/mmu/memory TU boundaries);
  `-march=native` (already the FMA fast path) / `-mtune=native`; PGO. Pure
  measurement work.
- **17. TLB entry shape** (`mmu.c:14` `TLBEntry` is ~25 B → 32 B padded): pack to
  16 B (flags in low bits) to fit two per cache line; fold with a re-profile.
---

## Tried and rejected — do not repeat

- **Per-PC decoded-instruction cache: measured 5–6% *slower*** than plain
  re-decode (tag-check + cache footprint outweighed the cheap decode). Its viable
  evolution — basic-block predecode — is now the **`-pd`** and **`-jit`** tiers,
  so this idea is settled: block-granular, not per-PC.
- **GIC bool-array → bitmap conversion (the second half of #8): not needed.**
  After the `gic_set_irq` early-out, `gic_best`/`gic_update`/`timer_update`/
  `machine_tick` all fall below 0.28% of a 300M-insn `-pd` boot (callgrind,
  2026-07-19) — there is nothing left for a `u32`-bitmap `gic_best` to win.

## Suggested order

1. **#14** build-flag measurement pass.
2. **#17** TLB-entry pack — only if a fresh profile shows `tlb[]` misses.
