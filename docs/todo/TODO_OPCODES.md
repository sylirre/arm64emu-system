# arm64emu — Not-Implemented Instructions (TODO)

Static survey begun 2026-07-03; cleaned 2026-07-18 to list only what is still
unimplemented or approximate, **refreshed 2026-07-18 (evening)** after the
v8.1–8.4 ecosystem-extension batch, and **again 2026-07-18 (late)** after the
FEAT_MOPS + FPSR-flags batch. Item numbers are preserved for cross-doc
references. Companion doc: `TODO_OPTIMIZATIONS.md` (performance);
`TODO_BUGS.md` was deleted when its last item closed (see git history).

**Advertised feature contract** (`sysreg.c`): ARMv8.0 FP+AdvSIMD **plus
FEAT_FP16** (`ID_AA64PFR0=0x110022`), AES+PMULL, SHA1, SHA256+SHA512, SHA3,
CRC32, **FEAT_LSE**, **FEAT_RDM**, **FEAT_DotProd**, **FEAT_FHM**,
**FEAT_FLAGM+FLAGM2** (`ID_AA64ISAR0=0x21100110212120`), **FEAT_JSCVT**,
**FEAT_FCMA**, **FEAT_LRCPC+LRCPC2** (`ID_AA64ISAR1=0x211000`), and
**FEAT_MOPS** (`ID_AA64ISAR2=0x10000`). The m19/m20/m21 self-tests pin these
values. FPSR is live: cumulative IOC/DZC/OFC/UFC/IXC, sticky QC, and the
quiet/signaling FCMP-vs-FCMPE IOC split (m22).

**Implemented since the audit (see git history):**
- **All of former §1** (baseline holes): half-precision conversions and the
  full FEAT_FP16 arithmetic surface, scalar FRINTA/FRINTX/FRINTI, scalar
  FCVTAS/FCVTAU, URECPE/URSQRTE, vector by-element SQDMULH/SQRDMULH/SQDMULL,
  scalar saturating b/h/s shifts, scalar FCVTXN, FPCR/FPSR, and finally
  **CPACR_EL1.FPEN trapping** (EC 0x07, all three engines; `m18_fptrap.S`).
- **FEAT_LSE** (LDADD/…/SWP/CAS/CASP + LDAPR; `m13_lse.S`).
- **LDTR/STTR true unprivileged semantics** (EL0 permission view incl. the
  D-TLB tag) and **AT S1E1R/W, S1E0R/W → PAR_EL1** via a real stage-1 probe
  (`m17_at_unpriv.S`).
- **The 2026-07-18 v8.1–8.4 batch** (all differential-tested against
  qemu-aarch64 -cpu max *before* the ID nibbles flipped; `m19_v84ext.S`,
  `m20_ext_simd.S`): FEAT_LRCPC2 LDAPUR/STLUR, FEAT_RDM SQRDMLAH/SQRDMLSH,
  FEAT_DotProd SDOT/UDOT, FEAT_FCMA FCMLA/FCADD, FEAT_FHM FMLAL/FMLSL,
  FEAT_JSCVT FJCVTZS, FEAT_FLAGM/FLAGM2 CFINV/RMIF/SETF8/SETF16/AXFLAG/XAFLAG.
  Along the way: the RMIF/SETF-as-ADC misdecode (bits[15:10] now checked), the
  FMLAL-space and by-element-U lax decodes, vector FRINTX/FRINTI ignoring
  FPCR.RMode, and scalar FCVTNS rounding ties away instead of to even. (That
  last one covered the FP→GPR form only; the AdvSIMD scalar `Dd,Dn` form kept
  its own open-coded convert — ties-away and no FPToFixed flags — until
  2026-08-04, when it was routed through the same `fround_mode` +
  `fcvt_to_int` pair as the vector page.)
- **The 2026-07-18 late batch** (`m21_mops.S`, `m22_fpsr.S`): **FEAT_MOPS**
  CPYx/CPYFx/SETx — Option A register forms matching qemu stage for stage
  (P to the page boundary, M whole pages, E the tail), backward-overlap
  memmove direction, unprivileged (T) forms on the LDTR EL0-view path,
  the EC 0x27 mismatch exception, SCTLR_EL1.MSCEn EL0 gating; and **FPSR
  cumulative flags** (former §6 items 4+7): IOC/DZC/OFC/UFC/IXC via lazy
  sticky host-flag accumulation shared by all three engines, sticky QC on
  the saturating clamps, FCMPE/FCCMPE signaling-vs-quiet IOC, FRINTX IXC,
  convert/estimate/f16-narrow flags, and the FRECPS/FRSQRTS fused step with
  the 0*inf -> 2.0/1.5 special case. ("Every clamp" was optimistic: the ones
  reached through the shared `sat_*` helpers raised QC, but UQXTN, UQSHRN/
  UQRSHRN, SQSHRUN/SQRSHRUN, SQSHLU, SQDMULL and SUQADD's 64-bit arm clamped
  inline and silently. Closed 2026-08-04 with two value bugs found alongside:
  SQABS/SQNEG of INT64_MIN returned INT64_MIN instead of INT64_MAX, and
  SQSHL/SQRSHL by at least the element width returned 0 instead of
  saturating — the negative bound `min >> sh` decays to -1 and never reaches
  0, so a source of exactly -1 slipped past it. See `m22_fpsr.S` 63-86.)

**How gaps surface at runtime:** FP/SIMD fall-throughs log
`[fpsimd] UNIMPL 0x… at pc=…`; integer/system fall-throughs log only with `-d`.

---

## 1. Holes inside the advertised baseline

None known. §1.1–§1.9 are all closed (numbers retired, kept for cross-doc
references).

---

## 2. Unadvertised extensions (absent by design — implement *then* advertise)

Guests probe HWCAPs/ID registers, so none of these can crash today. Ordered by
ecosystem value if/when pursued. Lesson from the Alpine bring-up (crypto):
never flip an ID nibble before the implementation is differential-tested.

| Extension | What's missing | Why it might matter |
|---|---|---|
| FEAT_PAuth | PACIA/AUTIA/BRAA/RETAA/LDRAA… (non-hint forms UNDEF; hint forms already NOP correctly) | security hardening only; NOP-hint behavior is architecturally valid while unadvertised. |
| FEAT_BTI | hint-space, already NOPs — only the GP page-table bit + trap logic would be new | hardening. |
| FEAT_MTE | ADDG/SUBG (`dp_immediate` t==0x23 → UNDEF), IRG/GMI/SUBP, STG/LDG family, SETG* (UNDEFs inside the MOPS space) | debugging ecosystems. |
| SVE/SVE2, SME | whole 0x04/0x24 encoding majors (`exec_a64` default → UNDEF) | out of scope for now; huge. |
| BF16/I8MM | BFDOT/BFMMLA/SMMLA… | ML. |
| FEAT_LRCPC3 | LDAPUR/STLUR SIMD&FP forms, LDAP/STL post-index | niche; LRCPC2 already covers the C++11 story. |

**Deliberately omitted** (keep): SM3/SM4 — decodes reserved in the 0xce space,
intentionally UNDEF and unadvertised.

---

## 3. System instructions / registers: silent RAZ/WI worth knowing about

- **`AT S1E1R/W, S1E0R/W` are implemented** (stage-1 probe → PAR_EL1); the
  other AT variants (S1E2*, S12E*) remain no-ops and `SYSL` reads return 0 —
  fine while EL2 isn't modeled.
- **All TLBI/DC/IC ops accepted** but DC ZVA is the only one with behavior —
  correct for a coherent flat memory model; keep.
- **Unknown MRS reads as 0, unknown MSR ignored.** Practical, but silently wrong
  values can mask guest bugs — the `-d` log line is the only trace. PMU
  registers, OSLAR, ID_AA64ZFR0 etc. all take this path (fine).
- **MSR-immediate PAN/UAO/DIT/SSBS ignored** — consistent with not advertising
  them. CFINV/AXFLAG/XAFLAG have graduated out of this bucket (FEAT_FLAGM2).

---

## 4. Load/store simplifications (documented, deliberate — keep or note)

- **Acquire/release ordering bits are ignored** throughout (LDAR/STLR/LDAXR/
  LDAPR/LDAPUR/STLUR…) — correct by construction in an in-order, single-CPU
  interpreter.
- **Exclusive monitor** is address-match only, no size check on STXR vs LDXR;
  fine single-CPU, worth a comment if SMP ever lands.

---

## 5. Lax decodes: unallocated encodings that still execute *something*

The corruption-risk cases are fixed (CAS-as-LDAR, three-same-FP→FADD,
ldst_pair opc3 in **both** halves, RMIF/SETF-as-ADC, FMLAL-space-as-compare/
FADD, by-element U-field ignores). `m16_laxdecode.S` holds the regression
battery for everything tightened.

The 2026-08-04 sweep against arm64chroot closed the rest of the known list
and found one more corruption case, which is the reason to keep treating
"does something plausible" as a severity question rather than polish:

- **SIMD&FP load/store pair with `opc==3`** (corruption). The integer half
  rejected it; the SIMD&FP half derived `scale = opc + 2`, i.e. a 32-byte
  element for a 16-byte-max vector access, and handed that size to
  `mem_read`/`mem_write` — a 32-byte memcpy into an 8-byte stack slot one way
  and 24 bytes of emulator stack into guest memory the other. Reachable from
  a single guest word (`0xED4000A0`).
- **`opc==3` with `size==2`** in the single-register family (the "load
  signed, 32-bit result" column exists only for byte and halfword).
- **DP-immediate/DP-register RES0 fields** never checked: bitfield `opc==3`
  and `N != sf`, EXTR bits[30:29], add/sub extended-register bits[23:22],
  DP-3-source bits[30:29], CCMP `S`/bit10/bit4, CSEL `S`/bit11, and the
  32-bit move-wide `hw >= 2` in `pd_fill`.
- **The 0xce crypto page leaking into the AdvSIMD vector handlers** (wrong
  values, not merely lax): nine dispatch guards matched bits[28:24]==0x0e
  without testing bit31, which is RES0 across the vector page, and the
  SHA3/SHA512 page shares those bits. BCAX with Rm in {0,1} and Ra % 4 == 2
  executed as REV64 of Vn. See `m7_crypto.S` checks 92-99.

Every probe was cross-checked against qemu-aarch64 with a valid neighbour to
catch over-tightening.

---

## 6. Semantic approximations in *implemented* FP

Items 4 (FCMPE aliased to quiet FCMP) and 7's flag half (no FPSR cumulative
flags, no QC) are **fixed** — see the late-batch entry above and `m22_fpsr.S`.
What remains, all deliberate:

- **FPCR.FZ/DN/AH are ignored and NaN payloads are not bit-guaranteed** — the
  declared scope limitation (oracle validation runs with FZ=0). FPSR.IDC is
  therefore never set (correct while nothing flushes).
- **FPCR.RMode is honored by FRINT/converts but not by arithmetic** — FADD &
  co always round-to-nearest-even (host default). Non-RN arithmetic is
  essentially unused by real guests (`fesetround` callers).
- **Tininess is detected after rounding** for float/double on x86 hosts (ARM:
  before rounding): UFC can differ at exactly one boundary ULP. The manual
  f16 narrows use ARM's before-rounding rule.

---

## 7. Verification recipe

- **Oracle:** `qemu-aarch64` user-mode. Either one instruction per run with
  fixed register preloads (compare full register state), or the m13-style
  dual-mode trick: build the self-test with `-DUSERMODE` (exit code = x0) and
  run it under `qemu-aarch64 -cpu max` to validate every hand-computed constant
  against real semantics before trusting it as the emulator's reference.
  Regression anchors: `tests/asm/m4_fpsimd.S`, `m8_simd.S`, `m9_simd_int.S`,
  `m10_simd_scalar.S`, `m7_crypto.S`, `m13_lse.S`, `m19_v84ext.S`,
  `m20_ext_simd.S`, **`m21_mops.S`** (per-stage MOPS state pins; the EC 0x27
  ISS checks are emulator-only since qemu-user turns the exception into
  SIGILL), **`m22_fpsr.S`** (FPSR flag battery), and the FP16 battery
  (arm64chroot `tests/c/fp16_*`).
  Known oracle flaw: qemu ≤ 8.2 decodes the LDAPUR/STLUR imm9 as unsigned
  (fixed upstream), so those negative-offset checks are emulator-only.
- Any §2 extension: implement, differential-test the full encoding group, **then**
  flip the ID nibble/HWCAP — never before.
- After each change, boot the Alpine ISO and the initramfs image and grep the log
  for `UNIMPL` — the boot path is clean; keep it that way.
