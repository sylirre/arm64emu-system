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
(#4/#6/#7/#8/#11/#12) sit outside the per-instruction path and benefit every tier.

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

---

## Device / system — benefits all three tiers (highest ROI now)

### 4. Finer-grained TLB invalidation (TLBI by VA, then ASID tagging)
`sysreg.c:38` still flushes the whole TLB on *every* TLBI (`CRn == 8`). The flush
itself is now O(1) (#1), so only the **miss-rate** cost remains: Linux's per-page
`TLBI VAE1IS`/`VALE1IS` on every unmap/COW/mprotect invalidate all 4096 entries.
Staged fix: (1) decode VA-form TLBIs and invalidate just the one direct-mapped
entry (+ `g_fcache` if its page matches); (2) tag entries with ASID + nG and stop
flushing on TTBR0 writes so context switches retain entries. Correctness-
sensitive — validate with the AECOV differential method + `m2_mmu.S`/`m6_cow.S`.

### 6. Split tick cadence — stop syscalling every 1024 instructions
`machine_tick` (`platform.c:149-150`) still runs `pl011_rx_poll` → `read(2)` and
`virtio_net_poll` → `poll(2)` every `tick_mask+1 = 1024` instructions even when
there is never input. Keep `timer_update` on the fine tick (pure arithmetic in
deterministic mode); move UART/net polling to a coarse cadence (~128k–1M insns).
Trivial, and it helps all tiers.

### 7. Timer: deadline compare instead of periodic re-evaluation
`timer_update` (`timer.c:42`) recomputes both counters and calls `gic_set_irq`
twice every tick. The deadline helpers already exist (`timer_next_deadline_ticks`,
used for the WFI fast-forward) — cache `next_deadline` and reduce the periodic
work to one `icount >= deadline` compare, recomputed on any CNT* write.

### 8. GIC: stop scanning 256 IRQs on every update
`gic_set_irq` (`gicv2.c:33`) unconditionally calls `gic_update` → `gic_best`
(linear scan of 256 entries across five bool arrays) — 2×/tick from the timer,
plus every IAR/EOIR. Add an early-out when neither `pending` nor `line` changed;
replace the bool arrays with `u32` bitmaps scanned via `__builtin_ctz`; cache the
current best.

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

## Suggested order

1. **Device/system, all-tier, low-risk:** #6 (tick syscalls) + #8 (GIC early-out)
   + #7 (timer deadline) — one afternoon.
2. **#4** (TLBI by-VA / ASID) — the biggest remaining TLB-miss reducer;
   correctness-sensitive, differential-test it.
3. **#11/#12** — cheap cleanups now that the D-TLB carries them.
4. **#14** build-flag measurement pass.
5. Interpreter micro-ops (#9/#10/#13/#15/#3/#16/#17/#18): low priority — `-jit`
   is the speed path; do these only to lift the portable interpreter.
