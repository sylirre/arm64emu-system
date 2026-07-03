# arm64emu — Optimization Opportunities (TODO)

Survey of the interpreter/device hot paths, 2026-07-03. Items are ordered by
expected payoff ÷ effort. Line references are to the current tree (HEAD 2563105).

**Measure before/after every item.** Host-side: the callgrind method
(`valgrind --tool=callgrind --dump-instr=yes ./arm64emu -bios ... -maxinsn 300000000`,
then `callgrind_annotate`). Guest-side: `AEPROF=1` hot-PC histogram.
Benchmark workloads: (a) deterministic full boot to the initramfs shell with
`-maxinsn` fixed, wall-clock time; (b) in-guest `dd if=/dev/zero of=/dev/null bs=1M`
(memset/DC ZVA path); (c) in-guest `openssl speed sha256` (SIMD path).
Deterministic timer mode makes runs instruction-exact, so identical `-maxinsn`
runs are directly comparable.

---

## Tier 1 — structural, biggest wins

### 1. O(1) TLB flush via generation counter  *(measured: ~14% of boot runtime)*
- **Where:** `src/mmu.c:25` `tlb_flush_all()` — `memset(tlb, 0, sizeof(tlb))` over
  4096 × 24 B = **96 KB per flush**.
- **Why hot:** called from every `TTBR0/TTBR1/TCR/SCTLR` write (`src/sysreg.c:133-137`)
  and every TLBI of any kind (`src/sysreg.c:36`). The kernel context-switches and
  TLBIs constantly; a prior callgrind profile attributed ~14% of a boot to this memset.
- **Fix:** add `u32 gen` to `TLBEntry` and a global `g_tlb_gen`; an entry is valid
  iff `e->gen == g_tlb_gen`; flush = `g_tlb_gen++` (on wrap, do one real memset).
  Keep the `g_fcache.host = NULL` invalidation.
- **Effort:** small. **Risk:** none, purely mechanical.

### 2. Host-pointer data TLB (QEMU-style softmmu fast path)
- **Where:** `src/mmu.c:149-190` (`mem_read`/`mem_write`) + `src/memory.c:163-217`
  (`phys_read`/`phys_write`).
- **Why hot:** every guest load/store today costs: `mmu_translate` (hash + tag
  compare + `perm_ok` call containing a switch) → `phys_read` (two range compares,
  `last_bus_status` store) → `memcpy` **with runtime size** (not constant-foldable;
  compiles to a real memcpy call or byte loop) → caller reloads `last_bus_status`.
  That's two cross-TU calls plus a libc call per memory access.
- **Fix:** make the TLB entry carry `u8 *host_page` (RAM pages, and flash pages only
  while in `FL_ARRAY` read mode) plus **precomputed prot bits** — R/W/X for EL0 and
  EL1 both, derived once from AP/UXN/PXN at fill time. Hit path becomes:
  index → one tag compare → one prot-bit test → fixed-size load/store on
  `host_page + (va & 0xfff)`. `host_page == NULL` (MMIO, flash-in-CFI-mode) falls
  through to the current slow path.
- **Interactions / risks:**
  - Flash writes must *never* take the fast path (CFI command capture,
    `src/memory.c:206-208`); flash reads only in array mode — simplest is to
    invalidate the data TLB's flash entries on any CFI mode change, or just
    exclude flash entirely (firmware runtime is a tiny slice of total).
  - `g_watch` / `g_vawatch` / `AEDBG` watchpoints bypass: gate the fast path on a
    `g_mem_hooks == 0` global (same pattern as `g_debug_hooks`, `src/cpu.c:380`).
  - Storing per-EL prot bits also lets you **drop `el0` from the tag**
    (`src/mmu.c:120`), eliminating the current EL0↔EL1 re-walk thrash when the
    kernel touches user pages (copy_to/from_user) — today each EL alternation on
    the same VA page forces a full 4-level walk and entry replacement.
- **Effort:** medium. Expected: largest single speedup after #1; every executed
  load/store benefits.

### 3. Fixed-size specializations of guest memory access
(Subsumed by #2's fast path, but worth doing even standalone / for the slow path.)
- **Where:** `src/memory.c:170-179` and `:202-208` — `memcpy(&v, p, size)` with
  runtime `size`.
- **Fix:** `switch (size) { case 1: … case 2: … case 4: … case 8: … }` with direct
  unaligned u8/u16/u32/u64 loads (host is x86, unaligned OK; use `__builtin_memcpy`
  with *constant* sizes per case so GCC emits single mov instructions).
- **Effort:** trivial.

### 4. Finer-grained TLB invalidation (TLBI by VA, then ASID tagging)
- **Where:** `src/sysreg.c:36` — *every* TLBI variant (`CRn == 8`) calls
  `tlb_flush_all()`.
- **Why hot:** Linux issues per-page `TLBI VAE1IS`/`VALE1IS` on every unmap, COW
  resolution, and mprotect. Each one currently dumps all 4096 entries (and the
  fetch cache), so steady-state runs at a far higher TLB miss rate (each miss =
  up to 4 `phys_read`s in `walk()`).
- **Fix (staged):**
  1. Decode op1/CRm/op2 to distinguish VA-form TLBIs; `Xt` bits `[43:0]` hold
     `VA[55:12]` — invalidate just the one direct-mapped entry (and `g_fcache` if
     its page matches). ASID-form (`ASIDE1`) and ALL-forms keep the full flush.
  2. Tag entries with ASID (TTBR0 bits `[63:48]`) + a global/nG bit from the
     descriptor, and **stop flushing on TTBR0 writes** — context switches then
     retain kernel entries and other processes' entries, like real hardware.
- **Effort:** step 1 small; step 2 medium (needs the nG bit captured in `walk()`,
  `src/mmu.c:92-102`, and careful checks against the re-walk-on-permission-miss
  policy documented at `src/mmu.c:126-135`).
- **Risk:** correctness-sensitive; validate with the AECOV differential method and
  `tests/asm/m2_mmu.S`, `m6_cow.S`.

### 5. Batch the run loop (`cpu_run(c, budget)`)
- **Where:** `src/main.c:241-266` and `src/cpu.c:418-457`.
- **Why hot:** per retired instruction we pay: ticker increment + mask test + weak
  symbol test (`machine_tick`), a cross-TU call to `cpu_step`, `c->stop` check,
  `g_systrace` load+branch, FIQ/IRQ line checks, `halted` check, debug-hook branch,
  `icount++`, StepResult branch, `halted` re-check, `max_insn` check. That's ~a
  dozen branches of pure loop administration around a ~few-dozen-instruction
  workload (the decode+execute itself).
- **Fix:** add `u64 cpu_run(CPU *c, u64 budget)` in cpu.c that runs the
  fetch/decode/execute core in a tight loop, checking only `c->stop`/`c->halted`,
  and returns retired count. Hoist per-1024 concerns (machine_tick, IRQ delivery
  re-check, max_insn) into the outer loop in main.c. IRQ lines only change inside
  `machine_tick` or MMIO writes performed by the guest itself, and DAIF only via
  MSR — so it is sufficient to (re)check IRQ delivery on loop entry and have those
  three sites set a `c->check_irq` flag that breaks the inner loop (or simply keep
  one combined `(irq_line|fiq_line) & ~daif`-style predicate as the single per-insn
  branch). Keep the current single-step path for `g_debug_hooks`.
- **Effort:** small-medium. Also enables #7's deadline check placement.

---

## Tier 2 — significant, localized

### 6. Stop making host syscalls every 1024 instructions
- **Where:** `machine_tick` (`src/platform.c:130-134`), called every
  `tick_mask+1 = 1024` instructions from `src/main.c:242`:
  - `pl011_rx_poll` → `tty_getchar` → **`read(2)`** on nonblocking stdin
    (`src/tty.c:55`) — one syscall per tick even when there is never input;
  - with `-net`: `virtio_net_poll` → slirp fill + **`poll(2)`** + dispatch
    (`src/devices/virtio_net.c:320-328`) — several syscalls per tick.
- **Why hot:** at interpreter speeds (~100+ MIPS) this is on the order of 10⁵
  syscalls/sec of pure overhead (worse with `-net`).
- **Fix:** split cadences. Keep `timer_update` on the fine tick (it is pure
  arithmetic in deterministic mode), move UART/net polling to a coarse tick
  (every ~128k-1M instructions ≈ ~1-10 ms — imperceptible for typing and fine for
  slirp). `AETICK` already exists for tuning; make the I/O cadence a second mask.
- **Effort:** trivial.

### 7. Timer: deadline compare instead of periodic re-evaluation
- **Where:** `timer_update` (`src/devices/timer.c:40-54`) recomputes both counters
  and calls `gic_set_irq` **twice** every tick.
- **Fix:** in deterministic mode the fire icount is exactly `cval - timer_skip`;
  cache `next_deadline` and reduce the periodic work to one `icount >= deadline`
  compare (fits naturally in #5's outer loop). Recompute the deadline on any CNT*
  write — the hook already exists (`src/sysreg.c:175`) — and on WFI fast-forward.
- **Effort:** small.

### 8. GIC: stop scanning 256 IRQs on every update
- **Where:** `gic_best` (`src/devices/gicv2.c:11-20`) — linear scan of 256 entries
  across five `bool` arrays; invoked by `gic_update` from every `gic_set_irq`
  (2×/tick from the timer), every IAR read and EOIR write.
- **Fix (any/all):**
  - early-out in `gic_set_irq` when neither `pending` nor `line` changed
    (the common case: timer line steady);
  - replace the bool arrays with `u32` bitmaps (8 words) and scan
    `pending & enabled & ~active` via `__builtin_ctz`, consulting priority only
    for set bits;
  - cache the current best and recompute only when a contributing bit changes.
- **Effort:** small. (Bitmaps also shrink GICD register reads to direct word loads,
  `src/devices/gicv2.c:48-88`.)

### 9. Skip NZCV computation on non-flag-setting ADD/SUB
- **Where:** `add_with_carry` (`src/decode.c:24-47`) computes N/Z/C/V — including
  the `__int128` signed-overflow path — for **every** ADD/SUB immediate/register
  (`src/decode.c:146-157`, `:240-258`), then the caller discards the flags when
  `S == 0`. Plain ADD/SUB are among the most frequent instructions in any trace.
- **Fix:** branch on `S` first: non-S path is just `n + imm` / `n - imm`.
  While there, replace the `__int128` C/V derivation with the standard bit tricks
  (`C = res < x` chain for carry; `V = ((x ^ ~y) & (x ^ res)) >> 63`).
- **Effort:** trivial.

### 10. Memoize `decode_bitmasks`
- **Where:** `src/decode.c:102-127`; executed by every logical-immediate *and*
  every UBFM/SBFM/BFM — i.e. **every LSL/LSR/ASR/UXTB/SXTW-by-immediate** — with a
  clz, rotates, and an up-to-64-iteration replication loop each time.
- **Fix:** the inputs are 13 bits (N:immr:imms) + sf. Lazy table:
  `struct { u64 wmask, tmask; u8 valid_sf_mask; } cache[8192]` filled on first use
  (~128 KB, or compute both sf variants at startup once).
- **Effort:** small.

### 11. DC ZVA: one translation + host memset
- **Where:** `sys_op` (`src/sysreg.c:38-43`) implements ZVA as **8 × `mem_write(8B)`**
  = 8 full TLB lookups + 8 phys dispatches per instruction.
- **Why hot:** kernel `clear_page` and musl `memset` use DC ZVA in tight loops
  (a page clear = 64 ZVAs = 512 translations today).
- **Fix:** translate the 64-B-aligned base once (it cannot cross a page), then
  `memset(host_ptr, 0, 64)` on the RAM fast path; fall back for MMIO. With #2 this
  falls out of the write-TLB naturally.
- **Effort:** trivial.

### 12. Single-translation 16-byte and pair accesses
- **Where:** `mem_read128`/`mem_write128` (`src/mmu.c:233-245`) do two independent
  8-byte `mem_read`s (2 TLB lookups); `ldst_pair` (`src/decode.c:488-527`) same;
  `ldst_vector_multi` LD1/ST1 (`src/decode.c:594-639`) does this per register —
  guest `memcpy` (NEON, 4×Q per iteration) pays 8 translations per 64 B.
- **Fix:** when `(va & 0xfff) <= 0x1000 - len`, translate once and do one host
  copy; keep the split path for page-crossers. Best layered on #2's host-pointer
  hit path.
- **Effort:** small-medium.

### 13. Per-EL fetch cache
- **Where:** `g_fcache` (`src/mmu.h:29-51`) is a single entry tagged with `el0`;
  every syscall, exception, and ERET misses it both ways.
- **Fix:** two entries indexed by `(c->el != 0)`. Cheap; helps syscall-heavy loads.
- **Effort:** trivial.

---

## Tier 3 — build, dispatch, micro

### 14. Build flags
- Current: `-O2`, no LTO (`Makefile:6`). Try, in order, measuring each:
  `-O3`; **`-flto`** (the per-instruction path crosses main.c → cpu.c → decode.c →
  mmu.c → memory.c TU boundaries; LTO lets the whole thing inline);
  `-march=native` (or `-mtune=native` for distributable builds); PGO
  (`-fprofile-generate` boot run → `-fprofile-use`). Keep `-fno-math-errno`.
- **Effort:** trivial; pure measurement work.

### 15. Second-level decode dispatch
- The top-level `exec_a64` switch (`src/decode.c:804-818`) is fine; the group
  handlers are sequential if-chains: `dp_immediate` ~7 compares, `branch_system`
  puts BR/RET/BLR behind ~6 compares, `loads_stores` tests exclusives before the
  far-hotter register/pair forms (`src/decode.c:696-709`), and `exec_fpsimd`
  walks ~25 predicate tests (`src/exec_fpsimd.c:2050-2138`).
- **Fix:** reorder chains by dynamic frequency (register/pair loads first,
  BR/RET earlier); convert to `switch` on extracted fields where the encodings
  allow jump tables; mark `undefined`/`fpsimd_undef`
  `__attribute__((cold, noinline))`; `likely()` the hit paths.
- **Effort:** small, incremental.

### 16. Keep debug hooks out of the hot path
- `mem_write` tests `g_vawatch` on every store (`src/mmu.c:168`); `phys_read` has a
  `g_dbg >= 2` DTB-range check (`src/memory.c:165-169`); `phys_write` tests
  `g_dbg >= 3` and `g_watch` (`src/memory.c:193-201`).
- **Fix:** fold into one `unlikely(g_mem_hooks)` guard set at startup (mirror of
  `g_debug_hooks`), single branch per access — or compile them out unless
  `-DAEDEBUG`.
- **Effort:** trivial.

### 17. TLB shape
- 24-B entries (4096 direct-mapped, `src/mmu.c:11-20`) straddle cache lines and
  alias hot kernel/user pages. Pack to 16 B (tag with flags in low bits) or pad to
  32 B; consider 2-way sets or folding an EL bit into the index. Do together
  with #2 (entry layout changes anyway).

### 18. CRC32 instructions
- `crc32_step` (`src/decode.c:72-79`) is bitwise, 8 steps/byte; ext4 metadata
  checksumming uses CRC32C heavily. Use slicing-by-8 tables, or the x86 `crc32`
  instruction for the CRC32C polynomial (exact match) via `-msse4.2` intrinsics.
- **Effort:** small; only matters for disk-heavy workloads.

---

## Tried and rejected — do not repeat

- **Per-PC decoded-instruction cache: measured 5-6% *slower*** than plain
  re-decode (tag check + cache footprint cost more than decode saves; decode is
  already cheap relative to the memory path). The viable evolution of this idea
  is **basic-block predecode** (translate a straight-line block once into an
  array of `(handler, pre-extracted operands)` and execute arrays; invalidate on
  the same contract as `g_fcache`/TLB flushes), ideally behind a `-jit`-style
  opt-in flag. Substantially more work: needs self-modifying-code invalidation,
  cross-page block handling, and exact `cur_insn_pc` reconstruction for faults —
  only attempt after Tier 1 lands and if the interpreter is still the bottleneck.

## Suggested order of attack

1. #1 (gen-counter flush) + #6 (tick syscalls) + #9 (flag elision) + #11 (DC ZVA)
   + #16 — one afternoon, all low-risk, likely ~20-30% combined.
2. #3 → #2 (host-pointer TLB, the big one) with #17 folded in.
3. #4 (TLBI VA / ASID) — biggest remaining TLB-miss reducer.
4. #5 + #7 + #8 (loop batching, timer deadline, GIC bitmaps).
5. #10, #12, #13, #14, #15 as cleanup passes, re-profiling between each.
