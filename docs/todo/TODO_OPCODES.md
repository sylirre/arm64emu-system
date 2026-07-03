# arm64emu — Not-Implemented Instructions (TODO)

Static survey of `src/decode.c`, `src/exec_fpsimd.c`, `src/sysreg.c`, 2026-07-03
(HEAD 2563105). Organized by risk: what can UNDEF *today* under the features the
emulator advertises, then what is safely absent because the ID registers don't
advertise it, then implemented-but-approximate semantics worth fixing.

**How gaps surface at runtime:** FP/SIMD fall-throughs always log
`[fpsimd] UNIMPL 0x… at pc=…` (`exec_fpsimd.c:12`). Integer/system fall-throughs
log **only with `-d`** (`decode.c:16`) — consider an `AEUNDEF=1` env to log them
unconditionally too, so field reports carry the encoding.

**Advertised feature contract** (`sysreg.c:59-67`): baseline ARMv8.0 FP+AdvSIMD
(`ID_AA64PFR0=0x22`, FP/AdvSIMD nibbles = 0), plus AES+PMULL, SHA1, SHA256+SHA512,
SHA3, CRC32 (`ID_AA64ISAR0=0x100012120`). `ID_AA64ISAR1=0`. Everything in §1
is *inside* that contract — a guest is entitled to use it without probing.

---

## 1. Holes inside the advertised baseline (latent SIGILL/panic — fix first)

### 1.1 Half-precision *conversions* (mandatory even without FEAT_FP16)
FP16 arithmetic is correctly unadvertised, but **storage conversions to/from
half are baseline ARMv8.0** and currently UNDEF:
- **Scalar `FCVT`** between H↔S and H↔D, both directions:
  - from-half rejected by the blanket `ftype != 0 && ftype != 1` guard
    (`exec_fpsimd.c:225`);
  - to-half (1-source opcode 0x7) missing from the FP-dp1 switch
    (`exec_fpsimd.c:334-346`).
- **Vector `FCVTL/FCVTL2` .4h→.4s and `FCVTN/FCVTN2` .4s→.4h** — the `sz==0`
  (half) variants explicitly UNDEF (`exec_fpsimd.c:1086`, `:1093`).

Emitted by any `__fp16`/`_Float16` code, image/audio libraries, and GCC
autovectorization. Highest-priority opcode work in this file.

### 1.2 Scalar FP round-to-integral: FRINTA / FRINTX / FRINTI
FP-dp1 switch (`exec_fpsimd.c:334-346`) implements FRINTN/P/M/Z (opc 0x8-0xb)
but UNDEFs opc 0xc/0xe/0xf. libm `round()`/`rint()`/`nearbyint()` compile
directly to these — likely the first thing a math-using app trips on.
(The vector forms already exist in `simd_two_misc_fp`; reuse `fround_mode`.)

### 1.3 Scalar FCVTAS / FCVTAU (FP → GPR, round-to-nearest-away)
The FP↔int convert block handles opcode≤1 for rmode N/P/M/Z but UNDEFs
opcode 4/5 (`exec_fpsimd.c:252-271`). Emitted by `lround()`/`llround()`.
Vector forms exist (`simd_two_misc_fp` keys 0x1c/0x5c) — port the same path.

### 1.4 URECPE / URSQRTE (vector unsigned estimates)
`simd_two_misc_fp` keys 0x3c/0x7c fall to UNDEF (`exec_fpsimd.c:1135`).
Rare (fixed-point DSP), but baseline.

### 1.5 Vector-form by-element saturating multiplies
`simd_indexed` (`exec_fpsimd.c:810-846`) implements MUL/MLA/MLS and
S/UMULL/MLAL/MLSL by element, but UNDEFs the **vector** forms of
`SQDMULH/SQRDMULH` (opc 0xc/0xd) and `SQDMULL/SQDMLAL/SQDMLSL` (opc 0xb/0x3/0x7)
by element. The **scalar** forms are already implemented
(`simd_scalar_indexed`, `exec_fpsimd.c:2017-2047`) — lift those kernels to the
lane loop. Common in fixed-point audio codecs.

### 1.6 Scalar saturating left shifts at b/h/s widths
`simd_scalar_shift` UNDEFs all non-narrowing shifts unless `immh&8` (D-form)
(`exec_fpsimd.c:1518`) — but scalar `SQSHL/UQSHL/SQSHLU #imm` are defined for
b/h/s too. The vector kernel (`simd_shift_imm` cases 0x0e/0x0c) already handles
all widths.

### 1.7 Scalar FCVTXN
Scalar two-misc U=1 opcode 0x16 (double→single, round-to-odd) falls through
`simd_scalar_cvt` to UNDEF (`exec_fpsimd.c:1347`). (Vector FCVTXN executes but
as plain FCVTN — see §5.)

### 1.8 FPCR / FPSR are RAZ/WI
`cpu.h:51` declares `fpcr, fpsr` but nothing reads or writes them; MRS/MSR of
FPCR/FPSR hit the sysreg default (read 0 / ignore, `sysreg.c:119-124,166-170`).
Consequences: `fesetround()` silently does nothing (host stays round-to-nearest),
`fetestexcept()` always sees 0, FPCR.FZ/DN unimplementable. At minimum, **store
and return the written values** (so context save/restore round-trips) and honor
FPCR.RMode in the convert/round helpers; exception *flags* can stay unset (they
already are — see §6).

### 1.9 CPACR_EL1.FPEN trapping never enforced
`fp_trapped` (`cpu.h:52`) is never set or consulted; `exec_a64` dispatches to
`exec_fpsimd` regardless of CPACR (`decode.c:810-815`). Modern Linux restores
FP state on the return-to-user path so it boots anyway, but kernels/hypervisors
that rely on the EL0 FP trap for lazy save/restore would silently see stale
vector registers. Fix: check CPACR_EL1.FPEN (and EL) at the FP/SIMD dispatch
and raise `EC_FP_ACCESS` (0x07).

---

## 2. Unadvertised extensions (absent by design — implement *then* advertise)

Guests probe HWCAPs/ID registers, so none of these can crash today. Ordered by
ecosystem value if/when pursued. Lesson from the Alpine bring-up (crypto): never
flip an ID nibble before the implementation is differential-tested.

| Extension | What's missing | Why it might matter |
|---|---|---|
| **FEAT_LSE** (`ISAR0.ATOMIC`) | LDADD/LDCLR/LDEOR/LDSET/LDSMAX…/SWP/CAS/CASP (+A/L/AL) — land in `ldst_register`'s `BITS(11,10)!=2` reject, `decode.c:467` | Biggest one: `-march=armv8.1+` userlands and `-moutline-atomics` fast paths; some prebuilt containers assume it. Also removes ldxr/stxr loop overhead under emulation. |
| FEAT_LRCPC/LRCPC2 | LDAPR/LDAPUR/STLUR | C++11 acquire loads in newer toolchains (probed via hwcap). |
| FEAT_FP16 (full) | all half-precision *arithmetic*, scalar+vector (blanket rejects at `exec_fpsimd.c:225,415,807,1404,1502`) | ML/graphics workloads; large surface, do after §1.1. |
| FEAT_DotProd | SDOT/UDOT (three-same-extra space, undecoded) | quantized ML kernels. |
| FEAT_RDM | SQRDMLAH/SQRDMLSH | codecs. |
| FEAT_FCMA | FCMLA/FCADD | complex-number DSP. |
| FEAT_JSCVT | FJCVTZS | JavaScript engines (V8/JSC probe it). |
| FEAT_FHM | FMLAL/FMLSL | ML. |
| FEAT_FLAGM(2) | CFINV/RMIF/SETF8/SETF16/AXFLAG/XAFLAG | minor codegen wins. |
| FEAT_PAuth | PACIA/AUTIA/BRAA/RETAA/LDRAA… (non-hint forms UNDEF; hint forms already NOP correctly, `decode.c:784-789`) | security hardening only; NOP-hint behavior is architecturally valid while unadvertised. |
| FEAT_BTI | hint-space, already NOPs — only the GP page-table bit + trap logic would be new | hardening. |
| FEAT_MTE | ADDG/SUBG (`dp_immediate` t==0x23 → UNDEF), IRG/GMI/SUBP, STG/LDG family | debugging ecosystems. |
| FEAT_MOPS | CPYx/SETx memory ops | glibc 2.39+ uses when advertised. |
| SVE/SVE2, SME | whole 0x04/0x24 encoding majors (`exec_a64` default → UNDEF) | out of scope for now; huge. |
| BF16/I8MM | BFDOT/BFMMLA/SMMLA… | ML. |

**Deliberately omitted** (keep): SM3/SM4 — decodes reserved in the 0xce space,
intentionally UNDEF and unadvertised (`exec_fpsimd.c:1792,1836`).

---

## 3. System instructions / registers: silent RAZ/WI worth knowing about

- **`AT S1E1R/W` etc. are no-ops** and `SYSL` reads return 0 (`sysreg.c:185-188`);
  `PAR_EL1` just returns whatever was last MSR'd to it (`sysreg.c:84,143`). A
  guest using AT for VA→PA probing gets stale/zero PAR. Implement AT on top of
  `walk()` if a guest ever needs it (KVM-style code, some kexec/hibernate paths).
- **All TLBI/DC/IC ops accepted** but DC ZVA is the only one with behavior
  (`sysreg.c:36-46`) — correct for a coherent flat memory model; keep.
- **Unknown MRS reads as 0, unknown MSR ignored** (`sysreg.c:119-124,166-170`).
  Practical, but silently wrong values can mask guest bugs — the `-d` log line
  is the only trace. PMU registers, OSLAR, ID_AA64ZFR0 etc. all take this path
  (fine). FPCR/FPSR must graduate out of this bucket (§1.8).
- **MSR-immediate PAN/UAO/DIT/SSBS ignored** (`sysreg.c:31`) — consistent with
  not advertising them; note LDTR/STTR interplay in §5.

---

## 4. Load/store simplifications (documented, deliberate — keep or note)

- **LDTR/STTR (unprivileged)** execute as ordinary loads/stores with
  current-EL permissions (`decode.c:472`) — the "unprivileged" override isn't
  modeled. Harmless while PAN isn't modeled either (EL1 can already access
  user pages), but must be revisited together if PAN is ever advertised.
- **Acquire/release ordering bits are ignored** throughout (LDAR/STLR/LDAXR…)
  — correct by construction in an in-order, single-CPU interpreter.
- **Exclusive monitor** is address-match only (`decode.c:557-588`), no size
  check on STXR vs LDXR; fine single-CPU, worth a comment if SMP ever lands.

---

## 5. Lax decodes: unallocated/foreign encodings that execute *something*

These don't affect well-formed guests but pollute differential testing and can
turn a future guest's probe of an unimplemented extension into silent
corruption instead of a clean UNDEF:

- **CAS/CASP encodings execute as LDAR/STLR.** `ldst_exclusive` routes on
  `o2` alone (`decode.c:552-555`); CAS (o2=1, o1=1) should UNDEF while LSE is
  unadvertised. One-line guard: `if (o2 && o1) undefined(...)`. **Do this now**
  — an armv8.1 binary that skips the hwcap check would otherwise corrupt memory
  instead of faulting.
- **Unrecognized three-same-FP keys execute as FADD** — the key switch defaults
  to `op = FOP_ADD` (`exec_fpsimd.c:624`), so e.g. the FMLAL2 (FEAT_FHM)
  encodings silently add. Default should be `fpsimd_undef`.
- **`ldst_pair` non-vector opc==3** (unallocated) executes as a 32-bit pair
  (`decode.c:499`) instead of UNDEF.
- `INS (general)`/vector-copy ignores some must-be-one/zero bits; several SIMD
  groups match on fewer fixed bits than the spec (acceptable, but a sweep to add
  `undef` defaults where the switch currently "does something plausible" is
  cheap insurance).

---

## 6. Semantic approximations in *implemented* FP (differential-test targets)

Known deviations from the architecture; all are candidates for the
one-instruction differential harness (see §7):

1. **Ties handling on scalar N-mode rounds.** Scalar `FRINTN`
   (`exec_fpsimd.c:341`) and scalar `FCVTNS/FCVTNU` (`:256`) use `f_round`
   (ties-away) where the architecture says ties-to-even. The correct helper
   `f_round_even` already exists (`:961`) and the vector path uses it — switch
   the scalar path over. Observable: `x.5` inputs off by one.
2. **FMADD/FMSUB/FNMADD/FNMSUB are unfused** (`exec_fp_dp3`,
   `exec_fpsimd.c:412-431`): computed as `a + n*m` with double rounding. Use
   `__builtin_fma{,f}` (inlines to vfmadd with `-mfma`/`-march=native`; without
   it, needs libm — Makefile is deliberately libc-only, so gate on the build
   flag). Vector FMLA/FMLS (`fop_d/fop_s` MLA/MLS) have the same issue.
3. **Scalar 2-source FMAX/FMIN/FMAXNM/FMINNM** (`exec_fpsimd.c:360-363`) use
   plain `>`/`<`: wrong NaN propagation and ±0 ordering, and MAXNM==MAX. The
   correct kernels exist (`fop_d/fop_s` FOP_MAX/MIN/MAXNM/MINNM) — reuse them.
4. **FCMPE/FCCMPE aliased to quiet FCMP/FCCMP** (`exec_fpsimd.c:318-330,394`):
   signaling-NaN behavior identical to quiet since FP exceptions aren't
   modeled. Fine until FPSR flags exist (§1.8).
5. **Vector FCVTXN executes as FCVTN** (`exec_fpsimd.c:1092-1098`) —
   round-to-odd not implemented.
6. **FRINTX/FRINTI ignore FPCR.RMode** (both map to ties-even,
   `exec_fpsimd.c:1116`), and FRINTX never raises Inexact — follows from §1.8.
7. **No FPSR cumulative flags, no FZ/DN, no NaN-payload guarantees** — declared
   scope limitation (`exec_fpsimd.c:502-506,990-991`); revisit only if a guest
   demonstrably cares.
8. **SADDLV/UADDLV look wrong** (`exec_fpsimd.c:935-945`): elements are summed
   without sign-extension (SADDLV) and the result is masked to the *source*
   element size (`acc & emask`) instead of the 2×-wide destination. E.g.
   `UADDLV h0, v1.8b` with all-0xFF lanes should give 0x7F8, code yields 0xF8.
   Verify against QEMU and fix; suspect the differential generators never
   covered the across-lanes long forms.

---

## 7. Verification recipe

- **Oracle:** `qemu-aarch64` user-mode, one instruction per run: assemble the
  target insn with fixed register preloads, run under QEMU and under
  `arm64emu -bin`, compare full register state (the method used for the EXT and
  UCVTF fixes). `tests/scripts/diff_qemu.sh` diffs PC streams for control-flow;
  the per-register generators (gen_simd/int/scalar.py) lived in a session
  scratchpad and are gone — regenerate on demand, and consider committing them
  to `tests/scripts/` this time.
- **Coverage sweep for §1:** brute-force the encoding space per group (iterate
  the switch key bits), execute under both, and diff — catches both UNDEF gaps
  and lax decodes (§5) in one pass.
- **Regression anchors:** `tests/asm/m4_fpsimd.S`, `m8_simd.S`, `m9_simd_int.S`,
  `m10_simd_scalar.S`, `m7_crypto.S` (`make test`).
- After each §1 fix, boot the Alpine ISO and the initramfs image and grep the
  log for `UNIMPL` — the boot path is currently clean; keep it that way.

## Suggested order

1. §5 CAS guard (one line, prevents silent corruption) + §1.8 FPCR store/return.
2. §1.1 half conversions, §1.2 FRINTA/X/I, §1.3 FCVTAS/AU — small, high-exposure.
3. §6.1 ties-to-even and §6.3 scalar min/max — reuse existing helpers, quick.
4. §6.8 SADDLV/UADDLV investigation.
5. §1.4-1.7 remaining baseline SIMD stragglers.
6. §1.9 CPACR trap; then §2 extensions as demand appears (LSE first).
