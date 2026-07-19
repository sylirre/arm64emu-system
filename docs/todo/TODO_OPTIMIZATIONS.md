# arm64emu — Optimization Opportunities (TODO)

Survey of the interpreter/device hot paths begun 2026-07-03; **cleaned
2026-07-18** to drop what has landed and reframe the rest around the tiered
executor that now exists. Item numbers are preserved. Line references drift as
the tree moves — treat them as hints.

**Perf landscape:** the emulator now has three execution tiers — the plain
interpreter (portable reference), **`-pd`** (computed-goto direct-threaded, ~2.46×,
~90 MIPS), and **`-jit`** (native codegen, ~10×, ~370 MIPS). `-jit`/`-pd` bypass
the interpreter's per-instruction loop entirely, so the interpreter micro-ops
below (#9/#10/#13/#15) now speed only the portable interpreter and the `exec_a64`
helper fallback — **for raw speed, use `-jit`.** The device/system items
(#4/#11/#12) sit outside the per-instruction path and benefit every tier.

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

---

## Device / system — benefits all three tiers (highest ROI now)

### 4 stage 2. ASID-tagged TLB entries: measured, deferred
Stage 1 (TLBI by-VA, above) landed 2026-07-19. Stage 2 — tag entries with
ASID + nG and stop flushing on TTBR0 writes — was *measured and deferred*: an
instrumented full boot shows only **259** TTBR/TCR/SCTLR-write flushes total
(vs 26773 VA-TLBIs that stage 1 already made single-page), so its ceiling here
is a few hundred avoided O(1) flush+refill cycles per boot. It would also
change the D-TLB tag ABI shared with both JIT backends' inline probes
(`dtlb_ctxgen`) — high blast radius for negligible measured payoff. Revisit
only if a context-switch-heavy workload (many concurrent processes) profiles
TTBR0-write refills as hot.

### 11. DC ZVA: one translation + host memset
`sys_op` (`sysreg.c:40`) still does ZVA as 8 × `mem_write(8B)`. Each write now
hits the D-TLB fast path (#2), so the cost is 8 cheap host stores rather than 8
translations — but a single `memset(host_ptr, 0, 64)` on the RAM fast path is
still nicer (kernel `clear_page`/musl `memset` hammer this).

### 12. Single-translation 16-byte and pair accesses
`mem_read128`/`mem_write128` (`mmu.c:347`), `ldst_pair`, and `ldst_vector_multi`
still do two independent 8-byte accesses. With #2 these are two D-TLB hits (cheap)
rather than two full walks, but one probe + one host copy for the non-crossing
case is still a win on NEON `memcpy` (4×Q/iter).

---

## Interpreter micro — portable interpreter + `exec_a64` helper only

Lower ROI now that `-jit`/`-pd` exist; pursue only to speed the portable path.

- **9. Skip NZCV on non-flag-setting ADD/SUB** (`decode.c:148-157,240-258`):
  `add_with_carry` computes N/Z/C/V for every ADD/SUB even when `S==0` and the
  caller discards them. Branch on `S` first.
- **10. Memoize `decode_bitmasks`** (`decode.c:104`): a clz + rotates + up-to-64-
  iteration replication loop on every logical-immediate *and* every
  UBFM/SBFM/BFM (i.e. every LSL/LSR/ASR-by-immediate). Lazy 13-bit-keyed table.
- **13. Per-EL fetch cache** (`mmu.h:43` `g_fcache` is one entry tagged with
  `el0`): every syscall/exception/ERET misses it both ways. Two entries indexed
  by `(c->el != 0)`.
- **15. Second-level decode dispatch** (`decode.c` group handlers are sequential
  if-chains; `loads_stores` tests exclusives before the far-hotter register/pair
  forms): reorder by dynamic frequency, convert to jump-table switches where the
  encodings allow, `cold`-mark the `undefined` paths.
- **3. Fixed-size `memcpy` in `phys_read`/`phys_write`** (`memory.c:10,184,215`):
  still runtime-size `memcpy`. The D-TLB fast path (#2) covers the hot path;
  this only helps the MMIO/first-touch slow path — low value.
- **16. Consolidate mem-access debug hooks:** largely moot — the D-TLB fast path
  skips the `phys_read`/`phys_write` `g_dbg`/`g_watch` checks entirely, leaving
  one predicted-not-taken `g_vawatch` branch in `mem_write`. A single
  `unlikely(g_mem_hooks)` guard would tidy the last branch.

---

## Build / micro

- **14. Build flags** (`Makefile`, still `-O2`, no LTO): measure `-O3`; `-flto`
  (the per-instruction path crosses main/cpu/decode/mmu/memory TU boundaries);
  `-march=native` (already the FMA fast path) / `-mtune=native`; PGO. Pure
  measurement work.
- **17. TLB entry shape** (`mmu.c:14` `TLBEntry` is ~25 B → 32 B padded): pack to
  16 B (flags in low bits) to fit two per cache line; fold with a re-profile.
- **18. CRC32 via slicing-by-8 / hardware** (`decode.c:74` `crc32_step` is
  bitwise, 8 steps/byte): ext4 metadata checksums use CRC32C heavily. Use
  slicing-by-8 tables or the x86 `crc32` instruction (exact CRC32C match).

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

1. **#11/#12** — cheap cleanups now that the D-TLB carries them.
2. **#14** build-flag measurement pass.
3. Interpreter micro-ops (#9/#10/#13/#15/#3/#16/#17/#18): low priority — `-jit`
   is the speed path; do these only to lift the portable interpreter.
