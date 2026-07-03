# arm64emu — Incorrect Opcode Implementations & Other Bugs (TODO)

Full-source correctness audit, 2026-07-03 (HEAD 2563105). Every file in `src/`
and `src/devices/` was read. Companion docs: `TODO_OPCODES.md`
(unimplemented encodings), `TODO_OPTIMIZATIONS.md` (performance).
Ordered by severity within each section. Items marked **[verified on host]**
were reproduced with a standalone C test of the same expression.

---

## 1. Instruction-semantics bugs (wrong architectural results)

### 1.1 All FP→integer converts turn NaN into INT_MIN instead of 0  **[verified on host]**
Casting a NaN `double` to `s64`/`u64` is UB; on x86 it yields
`0x8000000000000000`. The architecture's FPToFixed returns **0** for NaN.
Affects every convert path, scalar and vector:
- scalar GPR forms `exec_fpsimd.c:262-267` (FCVT{N,P,M,Z}{S,U})
- scalar fixed-point `:296-303`
- the shared vector helper `fcvt_to_int` `:950-957` (all vector FCVT*)
- scalar-SIMD-register forms `:1277-1284`
- fixed-point vector/scalar shifts `:1413-1416`, `:1510-1513`

Observable in any guest that converts a NaN (e.g. `(int)strtod("nan",0)`
returns INT_MIN instead of 0 — musl/glibc don't mask this).
**Fix:** one `if (r != r) return 0;` at the top of each clamp (or centralize
all five sites onto `fcvt_to_int` first, which is worth doing anyway).

### 1.2 SQDMULH/SQRDMULH: INT_MIN×INT_MIN saturates the wrong way  **[verified on host]**
`s64 p = 2*sx(a,esize)*sx(b,esize)` overflows s64 when both operands are
`INT32_MIN` (2·2^62 = 2^63): UB, and on x86 wraps to INT64_MIN so the result
saturates to **0x80000000** where the architecture requires **0x7FFFFFFF**.
Three copies: vector `exec_fpsimd.c:773-774`, scalar `:1980-1981`, scalar
by-element `:2035-2038`. (The SQDMULL family is safe — it already uses
`__int128`.) **Fix:** compute in `__int128` like `sat_s128`, or special-case
the min×min pair.

### 1.3 Vector SSHR/USHR #64 (.2d) shifts by 64 — UB, returns operand unchanged
`exec_fpsimd.c:1429-1430`: `sx(a,64) >> (2*esize - immhb)` and
`(a & emask) >> shift` with shift == 64 when esize==64 and immhb==64
(`SSHR d, #64` / `USHR v.2d, #64` are valid encodings). x86 masks the shift
count → returns `a` instead of 0 / sign-fill. The **scalar** path already
guards this (`:1523-1526` clamps sh≥64); the vector path missed it. SRSHR/
SSRA/etc. are safe (they go through `vreg_shift`). **Fix:** same clamp.

### 1.4 SADDLV/UADDLV: result truncated to source width, sign ignored
`exec_fpsimd.c:935-945`: the `SADDLV/UADDLV` case (`opc 0x03`) reuses the
same-width accumulate loop — elements are not sign-extended for SADDLV, and
the final store masks with the **source** esize (`acc & emask`) instead of the
2×-wide destination element. `UADDLV h0, v0.8b` with all lanes 0xFF returns
0xF8 instead of 0x7F8. **Fix:** dedicated branch with `sx()` per element and a
`2*esize` result mask (destination is a single wide scalar lane).

### 1.5 Scalar "round to nearest" uses ties-away instead of ties-to-even
- `FRINTN` scalar: `exec_fpsimd.c:341` uses `f_round` (ties-away);
- `FCVTNS/FCVTNU` scalar-GPR: `:256` (`case 0: r = f_round(v)`);
- `FCVTNS/FCVTNU` scalar-SIMD form: `:1273`.

The correct helper `f_round_even` exists (`:961`) and the **vector** versions
already use it via `fround_mode(x, 0)` — so the same instruction gives
different results in scalar vs vector form for `x.5` inputs. **Fix:** switch
the three scalar sites to `f_round_even`.

### 1.6 Scalar 2-source FMAX/FMIN/FMAXNM/FMINNM: wrong NaN and ±0 handling
`exec_fpsimd.c:360-363` (double) and `:375-378` (float) use plain `>`/`<`
ternaries: a NaN operand propagates only sometimes (returns `b` whenever the
compare is false), FMAXNM/FMINNM are aliased to FMAX/FMIN outright (marked
`~`), and `FMAX(+0,-0)` returns the wrong zero. The correct kernels exist —
`fop_d`/`fop_s` FOP_MAX/MIN/MAXNM/MINNM (`:537-552`) — and are already used by
every vector form. **Fix:** call them from the scalar 2-source switch.

### 1.7 CAS/CASP encodings silently execute as LDAR/STLR
`decode.c:552-555`: `ldst_exclusive` branches on `o2` alone; CAS has
o2=1,o1=1. An armv8.1 binary that skips its HWCAP check gets **memory
corruption** (its compare-and-swap becomes a plain store-release) instead of a
clean SIGILL. **Fix (one line):** `if (o2 && o1) { undefined(c, insn); return; }`
while FEAT_LSE is unadvertised.

### 1.8 LDR (literal, SIMD&FP) is UNDEF — missed baseline instruction
`decode.c:531-532` (`ldst_literal`): `if (V) { undefined(...); return; }`.
`LDR Sd/Dd/Qd, label` is baseline ARMv8.0 and appears in hand-written NEON
assembly (constant pools). Addendum to `TODO_OPCODES.md` §1 — it was
missed there. **Fix:** opc 00/01/10 → 4/8/16-byte `vreg_load` from
`cur_insn_pc + off`.

### 1.9 Vector FCVTXN executes as FCVTN
`exec_fpsimd.c:1092-1098`: U is ignored, so round-to-odd narrowing produces
round-to-nearest results (double-rounding hazard — the entire point of FCVTXN).
Scalar FCVTXN is UNDEF (opcodes doc §1.7).

### 1.10 FMADD/FMSUB/FNMADD/FNMSUB (and vector FMLA/FMLS) are unfused
`exec_fpsimd.c:412-431` computes `a + n*m` with intermediate rounding;
same in `fop_d/fop_s` FOP_MLA/FOP_MLS (`:528-529`, `:562-563`). One-ulp
divergences from real hardware in FMA-contracted code. **Fix:**
`__builtin_fma{,f}` — inlines to hardware with `-march=native`/`-mfma`;
without those flags it needs libm, and the build is deliberately libc-only, so
gate it on the build flag.

---

## 2. Memory system / exception model

### 2.1 Translation walk never rejects out-of-range VAs
`mmu.c:60-76`: `walk()` uses the VA sign bit to pick TTBR0/TTBR1 and uses
`txsz` only to choose the starting level; VA bits above `64-txsz` are neither
checked nor required to be a sign extension. A VA outside the configured range
(e.g. bit 47 set under a 39-bit T0SZ) silently **aliases** into the table
(`(va >> shift) & 0x1ff` masks the high bits) instead of raising a level-0
translation fault. Consequence: wild userspace pointers passed to the kernel
can translate instead of producing the EFAULT the guest expects. **Fix:**
check `va<63:va_size>` is all-0 (TTBR0) / all-1 (TTBR1) up front; fault with
`FSC_TRANS_L0`.

### 2.2 ID_AA64MMFR0 advertises the 64 KB granule but only 4 KB is implemented
`sysreg.c:65` returns QEMU-A57's literal `0x1124`: TGran4 (bits[31:28]=0) =
supported ✓, but TGran64 (bits[27:24]=0) also reads "supported" while `walk()`
hard-codes the 4 KB layout and ignores `TCR_EL1.TG0/TG1`. A 64 KB-page kernel
(some distros ship them) would boot into garbage translation instead of
politely failing. **Fix:** return TGran64=0xF (and TGran16=0xF explicitly);
alternatively implement the other granules (not worth it).

### 2.3 TCR TBI/EPD bits ignored; tagged pointers break the TTBR split
`mmu.c:65-66` selects the regime purely on `(s64)va >= 0`. With
`TCR_EL1.TBI0=1` (Linux sets it for EL0), a tagged user pointer whose tag has
bit 63 set is routed to **TTBR1** and walked with raw tag bits. Any guest
process using tagged pointers (HWASan-style allocators) will fault or alias.
EPD0/EPD1 (walk-disable) are also ignored. **Fix:** when the matching TBIx bit
is set, sign-extend from bit 55 before regime selection and walking; honor
EPDx by faulting.

### 2.4 No PC- or SP-alignment faults
`mem_ifetch` (`mmu.h:40`) happily fetches from a misaligned PC (a wild
`BR x0` executes rotated garbage instead of a PC-alignment fault), and
SP-alignment (SCTLR.SA/SA0) is unchecked. Mostly a debuggability loss —
faults appear far from the wild jump. Cheap partial fix: `if (va & 3)` in the
ifetch slow path → PC alignment fault (EC 0x22).

### 2.5 HVC/SMC (the PSCI conduit) is reachable from EL0
`decode.c:749-754` calls `smccc_conduit` regardless of `c->el`, and
`psci.c` acts on it — guest **userspace** can power off or reboot the machine
(`svc`-less `hvc #0` with x0=0x84000009). Architecturally HVC is UNDEFINED at
EL0. **Fix:** `if (c->el == 0) { undefined(...); return; }` before the conduit.

### 2.6 WFE has no event register / SEV semantics
`decode.c:784-786` treats WFE exactly like WFI (halt until interrupt). Code
that relies on SEVL/WFE pairs or on the exclusive-monitor-clear event (spinlock
paths, `smp_cond_load_relaxed`) only makes progress because the timer tick
eventually fires — on a tickless idle guest that can become a multi-ms stall
or a hang. **Fix:** minimal event-register model: SEV/SEVL set it, WFE with it
set clears it and does not halt; monitor-clear sets it.

### 2.7 CNTP/CNTV TVAL reads not truncated to 32 bits
`sysreg.c:104,110`: TVAL is architecturally a signed 32-bit view; returning
the full 64-bit difference confuses nothing today (Linux writes TVAL, rarely
reads) but is wrong. `(u64)(s32)(cval - count)` fixes it.

Cross-references (already in the opcodes doc, they are also correctness bugs):
FPCR/FPSR RAZ/WI, CPACR.FPEN never traps, LDTR/STTR privilege, AT ops no-op
with stale PAR_EL1.

---

## 3. Device-model bugs

### 3.1 virtio-net RX: unbounded descriptor-chain walk → host infinite loop
`virtio_net.c:145-157`: the RX delivery loop follows `next` pointers with **no
chain-length bound** (TX at `:189-203` and blk/9p both bound with `n >= q_num`).
A malformed or malicious guest driver posting a cycle of zero-length
descriptors with F_NEXT set hangs the emulator process hard (`written` never
advances, loop never breaks). Also: descriptors are not checked for
`VIRTQ_DESC_F_WRITE` before being written to. **Fix:** count descriptors,
bail at `q_num`, and skip read-only descriptors.

### 3.2 virtio-net RX: used.len over-reports on short buffer chains
`virtio_net.c:159`: `push_used(..., flen)` publishes the full frame length
even when the chain had less room (`written < flen`). The guest driver will
read `len` bytes from a buffer we only partially filled. **Fix:** push
`written`.

### 3.3 PL031 RTC: setting the clock double-counts, and the alarm never fires
`pl031.c:20-23`: `DR = base + lr` — LR is architecturally a *load* of the
counter, not an offset. After the guest sets the time (`hwclock --systohc`
writes LR = current epoch), DR reads back `base + epoch` ≈ 2× the intended
time. **Fix:** keep `tick_offset = lr_written - now` and return
`now + tick_offset` (QEMU's model). Also `mr` is stored but never compared and
`ris` is never set (`:37`) — the RTC alarm (RTCALARM wakeups) silently never
fires; either implement the match interrupt or drop it from the DTB.

### 3.4 GICv2: ITARGETSR reads return 0
`gicv2.c:78-82` returns `g->target[]`, which resets to 0 and is never
initialized to the boot CPU mask. Banked ITARGETSR0-7 must read as 0x01 per
byte on this single-CPU system; Linux's `gic_get_cpumask()` reads them to find
its CPU mask and warns "GIC CPU mask not found" when they're zero (grep the
boot log). **Fix:** return 0x01 per byte for IRQs 0-31 (and honor writes for
SPIs, or read-as-written).

### 3.5 virtio-blk: offset arithmetic can wrap; FLUSH never requested
`virtio_blk.c:106,114`: `off = sector * 512` and the range check
`off + total > capacity * 512` can both wrap for a hostile `sector`
(~2^55), passing validation and issuing a huge-offset pread/pwrite (fails at
the syscall, but by luck, not by check). Compute in unsigned with explicit
`sector > capacity || total > (capacity - sector) * 512` form. Also
`VIRTIO_BLK_F_FLUSH` is not advertised (`:26`), so guests never send FLUSH and
`fsync` (`:146`) is dead code — fine for a dev tool, but note that guest
"sync" gives no host durability. Minor: on read error, `used_len` still counts
the zero-filled chunks (`:126`).

### 3.6 fw_cfg: item table can overflow
`fwcfg.c:48`: `set_item` does `f->items[f->n++]` with no bound against the
fixed `items[96]`. Unreachable today (only ~9 items are created) but a
landmine for the first person who adds per-file entries. One bounds check.

### 3.7 virtio-9p: truncated request yields a zero-length reply
`virtio_9p.c:787` returns 0 for a short header; `p9_handle:872` then pushes a
used element with `len 0` — the guest's request completes with no R-message,
and that tag hangs in the client forever. Reply `Rlerror(EINVAL)` instead
(needs the tag, so parse what's available or use tag 0xFFFF/NOTAG). Minor:
`h_readdir` with a `count` smaller than the first entry returns an empty
Rreaddir, which the client interprets as EOF (`:459`).

---

## 4. Lax decodes — unallocated encodings that execute *something*

One sweep, shared fix (`undefined()` defaults). These pollute differential
testing and mask future guest bugs; a few risk host-side UB:

- `EXTR` with sf=0 and imms≥32: **UB host shifts** (`decode.c:210-211`) —
  `l >> lsb` / `h << (32-lsb)` with out-of-range counts.
- `ldst_pair` non-vector opc==3 → executes as a 32-bit pair (`decode.c:499`).
- Unrecognized three-same-FP keys → execute as FADD
  (`exec_fpsimd.c:624`, `default: op = FOP_ADD`).
- `do_load` size=3/opc=3 (unallocated) → 64-bit load truncated to 32
  (`decode.c:423`).
- Vector `ldst_register` with opc&2 and size≠0 → treated as a Q access
  (`decode.c:452`).
- `extend_reg` accepts shift amounts 5-7 (`decode.c:405-411` callers);
  `shift_reg` accepts imm6≥32 for 32-bit ops (wraps).
- `PMUL` with size≠0 executes byte-PMUL per lane (`exec_fpsimd.c:743`).
- `simd_copy` imm5==0 (UNDEF) executes as a .d DUP (`exec_fpsimd.c:107`);
  SMOV/UMOV invalid size/Q combinations execute.
- `MOVK/MOVZ/MOVN` hw>1 with sf=0 executes (`decode.c:175-184`).
- `dp_register` 3-source ops requiring sf=1 execute in 32-bit mode.

---

## 5. Audited and found correct (don't re-flag)

- Load/store base-writeback is correctly **skipped when the access faults**
  (`decode.c:417-427,482-486,514`) — the historical line-438 execve bug's fix
  is intact everywhere, including vector forms.
- `EXT` operand order (Vm:Vn) matches the pseudocode (the b60bf34 fix).
- Exceptions clear the exclusive monitor (`exception.c:50`), IRQ/SVC preferred
  return addresses are right (`cpu.c:430-437`, SVC uses pc+4, aborts use
  `cur_insn_pc`), and FIQ is checked before IRQ.
- The TLB's "re-walk on permission miss" policy (`mmu.c:126-135`) is the
  correct behavior for AF/dirty-bit upgrades without TLBI.
- `add_with_carry` C/V flags are correct in both widths (verified by the m1
  tests); SDIV INT_MIN/-1 and div-by-zero follow the architecture
  (`decode.c:374-379`).
- virtio-blk/9p descriptor walks are properly bounded; 9p `..` clamping and
  root containment hold for lexical paths (symlink escape is documented as
  out of scope).

---

## 6. Verification recipes

- **§1 items:** single-instruction differential vs `qemu-aarch64` user-mode
  (assemble insn + fixed register preload, compare full state). Specific
  vectors: NaN through every FCVT form (1.1); `SQDMULH v0.4s` with both lanes
  0x80000000 (1.2); `USHR d0, d1, #64` and `SSHR v0.2d, v1.2d, #64` (1.3);
  `UADDLV h0, v0.8b` all-0xFF (1.4); FRINTN/FCVTNS on 0.5/1.5/2.5 (1.5);
  FMAX with one NaN operand, FMAX(+0,-0) (1.6).
- **§2.1/2.3:** in-guest: mmap a region, pass a pointer with bit 47/tag bits
  set to `read(2)` — expect EFAULT, not success.
- **§3.1:** only reachable from a custom driver; review-only fix, add the
  bound and eyeball TX/blk/9p parity.
- **§3.3:** boot Alpine, `hwclock --systohc && hwclock`, compare with `date`.
- **§3.4:** grep the kernel boot log for `GIC CPU mask not found`.
- Regression: `make test` (m1-m11) after each fix; the SIMD fixes belong in
  `tests/asm/m8_simd.S`/`m10_simd_scalar.S` as new cases.

## Suggested order

1. §1.7 CAS guard + §2.5 EL0 conduit guard — two lines, corruption/security.
2. §1.1 NaN converts (centralize on `fcvt_to_int`), §1.2 SQDMULH, §1.3 shift-64,
   §1.4 SADDLV — small, all differential-testable.
3. §3.1/3.2 virtio-net RX bounds, §3.3 PL031, §3.4 ITARGETSR.
4. §1.5/1.6 scalar FP tie/NaN fixes (reuse existing helpers), §1.8 LDR literal.
5. §2.1 VA range check, §2.2 MMFR0 granule nibbles, §2.7 TVAL.
6. §4 lax-decode sweep; §1.9/1.10 and §2.3/2.4/2.6 as polish.
