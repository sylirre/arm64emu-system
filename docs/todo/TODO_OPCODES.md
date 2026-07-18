# arm64emu — Not-Implemented Instructions (TODO)

Static survey begun 2026-07-03; **cleaned 2026-07-18** to list only what is still
unimplemented or approximate. Item numbers are preserved for cross-doc
references. Companion docs: `TODO_BUGS.md` (incorrect implementations),
`TODO_OPTIMIZATIONS.md` (performance).

**Advertised feature contract** (`sysreg.c`): ARMv8.0 FP+AdvSIMD **plus
FEAT_FP16** (`ID_AA64PFR0=0x110022`, FP/AdvSIMD nibbles = 1), AES+PMULL, SHA1,
SHA256+SHA512, SHA3, CRC32, **FEAT_LSE** (`ID_AA64ISAR0=0x100212120`).
`ID_AA64ISAR1=0`.

**Implemented since the audit (see git history):**
- **All of former §1** (baseline holes) except §1.9 below: half-precision
  conversions and the full FEAT_FP16 arithmetic surface (now advertised), scalar
  FRINTA/FRINTX/FRINTI, scalar FCVTAS/FCVTAU, URECPE/URSQRTE, vector by-element
  SQDMULH/SQRDMULH/SQDMULL, scalar saturating b/h/s shifts, scalar FCVTXN, and
  FPCR/FPSR (store/return + FPCR.RMode honored in the round helpers).
- **FEAT_LSE** implemented and advertised (LDADD/…/SWP/CAS/CASP + LDAPR;
  `tests/asm/m13_lse.S`, kernel logs "LSE atomic instructions").
- The former §5 lax decodes that executed something (CAS-as-LDAR, three-same-FP
  default→FADD, ldst_pair opc3) and the §6 FP approximations that were bugs
  (ties-to-even rounds, unfused FMA, scalar FMAX/FMIN, vector FCVTXN,
  SADDLV/UADDLV) — all fixed.

**How gaps surface at runtime:** FP/SIMD fall-throughs log
`[fpsimd] UNIMPL 0x… at pc=…`; integer/system fall-throughs log only with `-d`.

---

## 1. Holes inside the advertised baseline

### 1.9 CPACR_EL1.FPEN trapping never enforced
`fp_trapped` (`cpu.h:54`) is declared but never set or consulted; `exec_a64`
dispatches to `exec_fpsimd` regardless of CPACR. Modern Linux restores FP on the
return-to-user path so it boots anyway, but a kernel/hypervisor relying on the
EL0 FP trap for lazy save/restore would silently see stale vector registers.
Fix: check CPACR_EL1.FPEN (and EL) at the FP/SIMD dispatch and raise
`EC_FP_SIMD_TRAP` (0x07). (Also tracked in `TODO_BUGS.md` §2.)

---

## 2. Unadvertised extensions (absent by design — implement *then* advertise)

Guests probe HWCAPs/ID registers, so none of these can crash today. Ordered by
ecosystem value if/when pursued. Lesson from the Alpine bring-up (crypto): never
flip an ID nibble before the implementation is differential-tested.

| Extension | What's missing | Why it might matter |
|---|---|---|
| FEAT_LRCPC/LRCPC2 | **LDAPUR/STLUR** (the unscaled forms); LDAPR itself is already implemented in the LSE space. Not advertised (`ISAR1=0`). | C++11 acquire loads in newer toolchains (probed via hwcap). |
| FEAT_DotProd | SDOT/UDOT (three-same-extra space, undecoded) | quantized ML kernels. |
| FEAT_RDM | SQRDMLAH/SQRDMLSH | codecs. |
| FEAT_FCMA | FCMLA/FCADD | complex-number DSP. |
| FEAT_JSCVT | FJCVTZS | JavaScript engines (V8/JSC probe it). |
| FEAT_FHM | FMLAL/FMLSL | ML. |
| FEAT_FLAGM(2) | CFINV/RMIF/SETF8/SETF16/AXFLAG/XAFLAG | minor codegen wins. |
| FEAT_PAuth | PACIA/AUTIA/BRAA/RETAA/LDRAA… (non-hint forms UNDEF; hint forms already NOP correctly) | security hardening only; NOP-hint behavior is architecturally valid while unadvertised. |
| FEAT_BTI | hint-space, already NOPs — only the GP page-table bit + trap logic would be new | hardening. |
| FEAT_MTE | ADDG/SUBG (`dp_immediate` t==0x23 → UNDEF), IRG/GMI/SUBP, STG/LDG family | debugging ecosystems. |
| FEAT_MOPS | CPYx/SETx memory ops | glibc 2.39+ uses when advertised. |
| SVE/SVE2, SME | whole 0x04/0x24 encoding majors (`exec_a64` default → UNDEF) | out of scope for now; huge. |
| BF16/I8MM | BFDOT/BFMMLA/SMMLA… | ML. |

**Deliberately omitted** (keep): SM3/SM4 — decodes reserved in the 0xce space,
intentionally UNDEF and unadvertised.

---

## 3. System instructions / registers: silent RAZ/WI worth knowing about

- **`AT S1E1R/W` etc. are no-ops** and `SYSL` reads return 0 (`sysreg.c:193`);
  `PAR_EL1` just returns whatever was last MSR'd to it (`sysreg.c:86,147`). A
  guest using AT for VA→PA probing gets stale/zero PAR. Implement AT on top of
  `walk()` if a guest ever needs it (KVM-style code, some kexec/hibernate paths).
  (Also in `TODO_BUGS.md` §2.)
- **All TLBI/DC/IC ops accepted** but DC ZVA is the only one with behavior —
  correct for a coherent flat memory model; keep.
- **Unknown MRS reads as 0, unknown MSR ignored.** Practical, but silently wrong
  values can mask guest bugs — the `-d` log line is the only trace. PMU
  registers, OSLAR, ID_AA64ZFR0 etc. all take this path (fine). FPCR/FPSR have
  graduated out of this bucket (explicit sysreg cases now).
- **MSR-immediate PAN/UAO/DIT/SSBS ignored** — consistent with not advertising
  them; note the LDTR/STTR interplay in §4.

---

## 4. Load/store simplifications (documented, deliberate — keep or note)

- **LDTR/STTR (unprivileged)** execute as ordinary loads/stores with current-EL
  permissions (`decode.c:588`) — the "unprivileged" override isn't modeled.
  Harmless while PAN isn't modeled either, but must be revisited together if PAN
  is ever advertised.
- **Acquire/release ordering bits are ignored** throughout (LDAR/STLR/LDAXR/
  LDAPR…) — correct by construction in an in-order, single-CPU interpreter.
- **Exclusive monitor** is address-match only, no size check on STXR vs LDXR;
  fine single-CPU, worth a comment if SMP ever lands.

---

## 5. Lax decodes: unallocated encodings that still execute *something*

The corruption-risk cases (CAS-as-LDAR, three-same-FP→FADD, ldst_pair opc3) are
fixed. What remains is cheap-insurance polish: `INS (general)`/vector-copy and a
few SIMD groups match on fewer fixed bits than the spec, so some unallocated
encodings "do something plausible" instead of a clean UNDEF. Enumerated with
file:line in `TODO_BUGS.md` §4 (all minor, differential-noise only).

---

## 6. Semantic approximations in *implemented* FP

Known, benign deviations from the architecture (candidates for the
one-instruction differential harness):

4. **FCMPE/FCCMPE aliased to quiet FCMP/FCCMP** (`exec_fpsimd.c:383,428`):
   signaling-NaN behavior is identical to quiet, since FP exceptions aren't
   modeled. Fine until FPSR cumulative flags exist.
6. **Vector FRINTX/FRINTI ignore FPCR.RMode** (`exec_fpsimd.c:1644,1706` map to
   ties-even), and FRINTX never raises Inexact. The *scalar* forms now honor
   FPCR.RMode; only the vector forms remain fixed-mode.
7. **No FPSR cumulative flags, no FZ/DN, no NaN-payload guarantees** — a declared
   scope limitation (bit-exactness is validated against the qemu-aarch64 oracle
   with FPCR.FZ=0). Revisit only if a guest demonstrably cares.

---

## 7. Verification recipe

- **Oracle:** `qemu-aarch64` user-mode, one instruction per run — assemble the
  target insn with fixed register preloads, run under QEMU and `arm64emu -bin`,
  compare full register state. Regression anchors: `tests/asm/m4_fpsimd.S`,
  `m8_simd.S`, `m9_simd_int.S`, `m10_simd_scalar.S`, `m7_crypto.S`, and the FP16
  battery (arm64chroot `tests/c/fp16_*`).
- Any §2 extension: implement, differential-test the full encoding group, **then**
  flip the ID nibble/HWCAP — never before.
- After each change, boot the Alpine ISO and the initramfs image and grep the log
  for `UNIMPL` — the boot path is clean; keep it that way.
