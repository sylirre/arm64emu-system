# arm64emu — Incorrect Opcode Implementations & Other Bugs (TODO)

Full-source correctness audit begun 2026-07-03; **cleaned 2026-07-18** to list
only the bugs that are still open. Item numbers are preserved for cross-doc
references. Companion docs: `TODO_OPCODES.md` (unimplemented encodings),
`TODO_OPTIMIZATIONS.md` (performance).

**Resolved since the audit (see git history):**
- **§1 — every instruction-semantics bug** (NaN→INT_MIN converts, SQDMULH/SQRDMULH
  INT_MIN² saturation, vector SSHR/USHR #64, SADDLV/UADDLV, scalar ties-to-even
  rounds, scalar FMAX/FMIN NaN/±0, CAS-as-LDAR, LDR SIMD&FP literal, vector
  FCVTXN round-to-odd, unfused FMADD/FMLA).
- **§2.1–§2.7** — MMU out-of-range VA rejection + TCR TBI/EPD, MMFR0 granule
  nibbles, PC- and SP-alignment faults, HVC/SMC UNDEF at EL0, WFE event register;
  FPCR/FPSR store-and-return (no longer RAZ/WI); and CNTP/CNTV **TVAL reads now
  return the architectural signed-32-bit view** (`sysreg.c`).
- **§3.1–§3.7** — every device-model bug: virtio-net RX bounds, PL031 clock
  double-count **and** the RTCMR match/alarm interrupt, GICv2 ITARGETSR
  banked-IRQ mask, the virtio-blk offset-wrap range check **and now**
  `VIRTIO_BLK_F_FLUSH` advertisement (+ don't count a failed read chunk in
  `used_len`), the fw_cfg item-table bound, and the virtio-9p truncated-header
  reply **and** the tiny-`count` readdir that read as spurious EOF.
- **§4 — every lax decode**: vector `ldst_register` opc&2/size≠0, `extend_reg`
  shift 5–7, add/sub & logical shifted-register 32-bit imm6≥32, add/sub shifted
  ROR, 3-source multiplies with sf=0, `PMUL` size≠0, and AdvSIMD-copy reserved
  imm5/imm4/Q (imm5<3:0>==0, SMOV/UMOV/DUP bad size/Q) now UNDEF **consistently
  across the interpreter, `-pd`, and `-jit`**. Regression: `tests/asm/m16_laxdecode.S`.
- **Earlier §4** — EXTR (N/imms/bit21, closing the u32 shift UB), move-wide
  hw≥2/sf=0, ldst_register opc3/size3, ldst_pair opc3, three-same-FP default.

---

## 2. Memory system / exception model — documented approximations (still open)

These are deliberate simplifications, not active bugs in the advertised ISA
contract. None affects the Linux boot; revisit each only if a guest needs it.

- **CPACR_EL1.FPEN trapping is never enforced.** `fp_trapped` (`cpu.h:54`) is
  declared but never set or consulted; EL0 FP/SIMD accesses dispatch regardless
  of CPACR. Modern Linux restores FP on return-to-user so it boots, but a
  kernel/hypervisor relying on the EL0 FP trap for lazy save/restore would see
  stale vector state. Fix: check CPACR_EL1.FPEN at the FP/SIMD dispatch and raise
  `EC_FP_SIMD_TRAP` (0x07). *Carries boot-regression risk — gate carefully.*
- **LDTR/STTR execute with current-EL permissions** (`decode.c`, the
  unscaled/unprivileged addressing mode) — the "unprivileged" override isn't
  modeled. Harmless while PAN isn't modeled, but must be revisited together if
  PAN is ever advertised.
- **AT S1E1R/W etc. are no-ops**, `SYSL` reads return 0 (`sysreg.c`), and
  `PAR_EL1` returns whatever was last MSR'd to it (`sysreg.c`). A guest using
  AT for VA→PA probing gets a stale/zero PAR. Implement AT on top of `walk()` if
  a guest ever needs it (KVM-style code, some kexec/hibernate paths).

---

## 5. Audited and found correct (don't re-flag)

- Load/store base-writeback is correctly **skipped when the access faults**
  (`decode.c`) — the historical line-438 execve bug's fix is intact everywhere,
  including vector forms.
- `EXT` operand order (Vm:Vn) matches the pseudocode (the b60bf34 fix).
- Exceptions clear the exclusive monitor, IRQ/SVC preferred return addresses are
  right (SVC uses pc+4, aborts use `cur_insn_pc`), and FIQ is checked before IRQ.
- The TLB's "re-walk on permission miss" policy is the correct behavior for
  AF/dirty-bit upgrades without TLBI.
- `add_with_carry` C/V flags are correct in both widths; SDIV INT_MIN/-1 and
  div-by-zero follow the architecture.
- virtio-blk/9p descriptor walks are properly bounded; 9p `..` clamping and root
  containment hold for lexical paths (symlink escape is documented out of scope).

---

## 6. Verification

- **§4 lax decodes:** `tests/asm/m16_laxdecode.S` executes each newly-guarded
  encoding (derived from a valid neighbour and confirmed `<unknown>` by the
  toolchain's own decoder) behind a sync-exception handler and asserts EC_UNKNOWN,
  while interleaved valid neighbours must not trap. Passes under the interpreter,
  `-pd`, `-jit`, and the AArch64 backend under `qemu-aarch64`. For a broader
  sweep, brute-force each group against `qemu-aarch64` user-mode and diff.
- **§2.7:** program a large `CNTP_CVAL`, read `CNTP_TVAL`, check it against the
  signed 32-bit view (or rely on `m15_rtc` + the byte-identical full boot).
- **§3.x:** dev-tool severity; verified by build + code review + a clean full
  boot (`-drive`/`sync` for FLUSH, the 9p host harness for the readdir path).
- Regression: `make test` (m1–m16), `make test-jit`, the `-pd`/`-jit` boot
  consistency checkpoints, and `make test-jit-a64`. All remaining items above are
  documented approximations; there are **no known open correctness bugs in the
  advertised ISA contract.**
