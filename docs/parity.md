# Engine parity: interpreter vs `--pd` vs `--jit`

Audit of how the three execution engines stay semantically identical, what is
allowed to differ, and how parity is verified. Snapshot date: 2026-07-19.
Companion documents: `docs/jit.md` (JIT internals), `docs/pd.md` (`--pd` tier).

## The structural guarantee

The interpreter (`cpu_step` → `exec_a64`, `src/cpu.c` / `src/decode.c` /
`src/exec_fpsimd.c`) is the reference semantics. Both accelerated tiers are
transcriptions of it that fall back into it:

- `--pd`: anything `pd_fill` leaves `PD_GENERIC` dispatches to
  `L_GENERIC: exec_a64(c, insn)` (`src/jit/predecode.c`). The `NEXT` epilogue
  mirrors `cpu_step`'s tail exactly (icount, rare-event checks, fetch).
- `--jit`: anything the frontend does not translate becomes
  `IRO_CALL1 → jit_exec1 → exec_a64` (`src/jit/jit.c`), which replicates
  `cpu_step`'s set-PC / execute / `icount++` contract, including counting
  aborted instructions.

Divergence can therefore only hide in two places:

1. **The JIT's inlined surface** — integer ALU/bitfield/csel/ccmp/mul/div,
   lazy NZCV in host flags, inline D-TLB load/store probes and memory-run
   fusing, inline exclusives and CLREX, the hot-sysreg whitelist, inline
   CNTPCT/CNTVCT, and the `be_vop_ok` FP/SIMD whitelist.
2. **Coherence machinery** — block identity/chaining, the indirect-branch
   jcache, SMC store-tracking (`g_jit_code_bitmap` + D-TLB W-bit refusal),
   ctx tags `(pc<<3) | {EL0, MMU, SP bank, FPEN}`, flush/TLB generations.

`--pd` has almost no inlined-semantics surface: its native handlers are the
integer/branch/load-store classes plus FP *load/stores* (with their own
`FP_GUARD`); all FP data-processing, system and exception ops stay GENERIC.
SP-based memory ops are deliberately GENERIC so SP-alignment faults match.

## Deliberate punts (interpreter stays authoritative)

The frontend refuses these so host semantics can never leak
(`src/jit/frontend.c`):

- **FMADD/FMLA/FMLS families** — the interpreter uses a genuinely fused
  `__builtin_fma`; an inline mul+add would double-round.
- **FCMPE/FCCMPE** (signaling compares), and half-precision FMADD on x86
  (double rounding).
- **x86 only** (`be_vop_ok` divergences are per-backend and intentional):
  FCVTZS/FCVTZU to GPR (inline `cvttsd2si` branches would skip host
  invalid/inexact accumulation), FMAX/FMIN/FMAXNM/FMINNM (SSE min/max NaN/±0
  ordering differs from ARM), half-precision FMULX/FRECPE/FRSQRTE. The a64
  backend keeps these native (architecturally exact there).
- **NaN-gated FP arithmetic** (VC_F2/F3/VF3S…): computed inline, but a NaN
  result discards and re-runs via the helper (`vop_slowpath`) so the
  interpreter's operand-order-dependent NaN propagation is authoritative.
  These classes are "self-counting" (fast path bumps icount inline; the
  helper counts on the slow arm).
- **CASP**, byte/half signed LDSMAX/LDSMIN/LDUMAX/LDUMIN, **LD2/3/4** and
  single-lane vector loads, **MOPS** (EC 0x27 state machine), invalid
  bitfield/logical-imm encodings (helper raises the UNDEF), every `0xD5`
  system instruction outside the hot whitelist (may change translation
  state; ends the block), SVC/HVC/BRK/ERET.

Bug-for-bug quirks preserved by construction: address-only exclusive monitor
(no size check), faulting ST*XR leaves the monitor set, CAS space stays on
`exec_a64`, acquire/release ordering ignored (single CPU, in-order).

FPSR sticky flags work across engines because inline SSE/NEON emits and the
interpreter's helpers run on the same thread's host FP status word, folded
lazily by `fpsr_sync` (`src/exec_fpsimd.c`) only when the guest reads FPSR.

## Accepted deviations (documented, not bugs)

- **Tick cadence**: the interpreter polls devices every 1024 loop iterations,
  the fast tiers at block/slice boundaries. Deep in a boot the guest can read
  timer values at slightly different icounts — shifted printk timestamps and
  clock-derived values (e.g. the kernel audit stamp). Console content stays
  identical; `tests/run_bootlog_gate.sh` normalizes exactly these.
- **Same-page self-modification** (`--jit`): a store into the running block's
  page lets the stale tail run to the block end (≤128 insns) —
  architecturally permitted without IC IVAU+ISB.
- **Host FP semantics** (all engines equally, `docs/todo/TODO_OPCODES.md` §6):
  FPCR.FZ/DN/AH ignored, NaN payloads not bit-guaranteed, FPCR.RMode honored
  by FRINT/converts but not arithmetic, x86 tininess-after-rounding (one-ULP
  UFC boundary).
- **Debug facilities force the interpreter** (`--trace`, `--reg-trace`, watchpoints,
  AE* per-instruction hooks).

## Verification matrix

| Gate | Command | What it proves |
|------|---------|----------------|
| asm suite ×3 | `make test` / `make test-pd` / `make test-jit` | m1–m23 pass (`x0=0`) under every engine; values oracle-validated once via qemu-aarch64 (`-DUSERMODE` dual-mode files: m13, m19–m22) |
| consistency checkpoints | inside `test-pd` / `test-jit` | deterministic firmware+Linux boot (harness pins `AE_RTCLOCK=0`; runtime default is the host clock), byte-identical serial + final CPU state at 1M/4M/16M/64M/300M |
| full-boot log gate | `make test-jit-full` / `make test-pd-full` (`tests/run_bootlog_gate.sh`) | 1.6B-insn boot log identical after timestamp normalization — covers the whole boot, not just the UEFI phase the checkpoints sit in; **mandatory for frontend changes** |
| cross-engine fuzzing | `make fuzz-engines` (`tests/run_fuzz_engines.sh` + `tests/scripts/fuzz_gen.c`) | random blocks over the inlined surface, interpreter vs `--pd` vs `--jit` vs `--jit`+SLOWMEM/NOFUSE/NOVRA; phase 1 compares the HLT line natively-executed, phase 2 stops at the exact pre-HLT icount and compares the full register dump |
| a64 backend | `make test-jit-a64` (and `AE_RUNNER=qemu-aarch64 AE_EMU=./arm64emu-a64 tests/run_fuzz_engines.sh`) | the second backend stays executable and byte-identical under qemu-user |
| regression pins | `tests/asm/m23_jitpar.S` | the two fuzzer-found bugs below stay fixed, engine-agnostic |

Bisection knobs: `AEJIT_PDMAX` / `AEJIT_SLOWMEM` / `AEJIT_NOFUSE` /
`AEJIT_NOVRA` / `AEJIT_DUMP` (docs/jit.md) and `AEPD_MAX=N` for `--pd`
(dispatch only PD ops ≤ N natively; 0 = pure interpreter).

## Case studies: what the fuzzer caught (2026-07-19)

Both bugs passed every pre-existing gate (the checkpoints sit in the UEFI
phase; the boot-log gate strips the timestamps that would have shown drift).
15/2000 seeds diverged; both root causes were x86-JIT-only.

1. **Lazy-NZCV hole after FCSEL** (`backend_x86_64.c`, VC_FCSEL). An S-op
   whose next flag-relevant op is a consumer keeps NZCV lazily in host
   EFLAGS. The integer CSEL emit re-evaluates `flags_next_use` after
   consuming and materializes if the flag future is unknown; the FCSEL emit
   did not — so `SUBS ; FCSEL ; ORR ; … ; B.cond` let the plain ALU clobber
   EFLAGS while `be->fl` still claimed them live, and a later materialize or
   consumer read garbage flags. Symptom in the fuzz run: a `b.mi` went the
   wrong way, visible only as an icount/register drift. Fix: FCSEL now ends
   with the same `flags_next_use` re-check as integer CSEL.
2. **Uncounted NOPs** (`frontend.c`, `PD_NOP`). The frontend consumed
   NOP/hint/PRFM without `ir->ninsns++`, so NOPs retired in translated code
   were missing from icount: guest time (CNTVCT derives from icount) ran
   slow, and `--max-insn` overshot (a 600-NOP image blew ~200 insns past the
   limit). Fix: count them.

Lesson encoded in the matrix above: the structural guarantee makes the
*punted* surface safe by construction, so adversarial coverage only needs to
target the *inlined* surface — which is exactly what `fuzz_gen.c` weights.
A divergence report from `run_fuzz_engines.sh` names the diverging configs:
all-JIT-configs ⇒ frontend/emit semantics; SLOWMEM-only ⇒ D-TLB/mem-run
machinery; NOVRA-only ⇒ V-register cache; `--pd` too ⇒ shared predecode
classification (`pd_fill`).

## Known observability caveat (by design)

Inside a `--jit` block, natively-retired instructions are credited to
`c->icount` at block exits (cumulative `o->icnt`), not per instruction. A
mid-block `CALL1` helper therefore observes an icount that is stale by up to
the current block's native prefix (≤128). Architectural state at every block
boundary — and any `--max-insn` stop, via the interpret-tail — is exact; the
staleness is observable only through in-helper reads of icount-derived
values, and lands within the accepted tick-cadence deviation. The `[HLT]`
debug print is the visible instance (its icount can trail the interpreter's).
