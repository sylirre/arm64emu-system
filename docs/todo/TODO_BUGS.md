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
- **§2.1–§2.6** — MMU out-of-range VA rejection + TCR TBI/EPD, MMFR0 granule
  nibbles, PC- and SP-alignment faults, HVC/SMC UNDEF at EL0, WFE event register;
  and FPCR/FPSR are no longer RAZ/WI (they store and return).
- **§3.1–§3.4** — virtio-net RX bounds, PL031 clock double-count **and** the
  RTCMR match/alarm interrupt, GICv2 ITARGETSR banked-IRQ mask; plus the
  virtio-blk offset-wrap range check (§3.5) and the virtio-9p truncated-header
  reply (§3.7).
- **Most of §4** — EXTR (N/imms/bit21, closing the u32 shift UB), move-wide
  hw≥2/sf=0, ldst_register opc3/size3, ldst_pair opc3, and the three-same-FP
  default (was FADD) → all UNDEF now.

---

## 2. Memory system / exception model

### 2.7 CNTP/CNTV TVAL reads not truncated to 32 bits
`sysreg.c:108,114`: TVAL is architecturally a signed 32-bit view, but the read
returns the full 64-bit `(s64)(cval - count)` difference (the write side already
sign-extends its 32-bit input). Confuses nothing today (Linux writes TVAL,
rarely reads it) but is wrong. `(u64)(s32)(cval - count)` fixes the read.

Also still open (cross-referenced from the opcodes doc — documented
approximations more than active bugs):
- **CPACR_EL1.FPEN trapping is never enforced.** `fp_trapped` (`cpu.h:54`) is
  declared but never set or consulted; EL0 FP/SIMD accesses dispatch regardless
  of CPACR. Modern Linux restores FP on return-to-user so it boots, but a
  kernel/hypervisor relying on the EL0 FP trap for lazy save/restore would see
  stale vector state. Fix: check CPACR_EL1.FPEN at the FP/SIMD dispatch and raise
  `EC_FP_SIMD_TRAP` (0x07).
- **LDTR/STTR execute with current-EL permissions** (`decode.c:588`) — the
  "unprivileged" override isn't modeled. Harmless while PAN isn't modeled, but
  must be revisited together if PAN is ever advertised.
- **AT S1E1R/W etc. are no-ops**, `SYSL` reads return 0 (`sysreg.c:193`), and
  `PAR_EL1` returns whatever was last MSR'd to it (`sysreg.c:86`). A guest using
  AT for VA→PA probing gets a stale/zero PAR. Implement AT on top of `walk()` if
  a guest ever needs it (KVM-style code, some kexec/hibernate paths).

---

## 3. Device-model bugs

### 3.5 virtio-blk: FLUSH never advertised (dead fsync path)
`VIRTIO_BLK_F_FLUSH` is not in `BLK_FEATURES` (`virtio_blk.c:28,257`, only
`VIRTIO_F_VERSION_1`), so guests never send `VIRTIO_BLK_T_FLUSH` and the `fsync`
path is dead code — a guest "sync" gives no host durability. Fine for a dev tool,
but worth noting. Minor: on a read error, `used_len` still counts the
zero-filled chunks. (The offset-wrap range check is now overflow-safe.)

### 3.6 fw_cfg: item table can overflow
`fwcfg.c:50`: `set_item` does `f->items[f->n++]` with no bound against the fixed
`items[96]`. Unreachable today (~9 items are created) but a landmine for the
first person who adds per-file entries. One bounds check.

### 3.7 virtio-9p: readdir with a tiny count returns EOF
The truncated-header case now replies `Rlerror(EINVAL)`. Minor remaining:
`h_readdir` with a `count` smaller than the first entry returns an empty
Rreaddir, which the client interprets as end-of-directory (`virtio_9p.c:459`).

---

## 4. Lax decodes — unallocated encodings that still execute *something*

These pollute differential testing and can mask a future guest's bug; none of
the ones below risk host UB (the EXTR and shift-UB cases are fixed). All minor —
one sweep adding `undefined()` defaults would close them:

- Vector `ldst_register` with `opc&2` and `size≠0` → treated as a Q access
  (`decode.c:563`).
- `extend_reg` accepts shift amounts 5–7; `shift_reg` accepts imm6≥32 for 32-bit
  ops (the amount just wraps) (`decode.c:83,417`).
- `PMUL` with `size≠0` executes byte-PMUL per lane (`exec_fpsimd.c:1108`; PMUL is
  defined only for `.8b`/`.16b`).
- `simd_copy` imm5==0 (UNDEF) executes as a `.d` DUP (`exec_fpsimd.c:109`);
  SMOV/UMOV invalid size/Q combinations execute.
- `dp_register` 3-source ops requiring sf=1 execute in 32-bit mode
  (`decode.c:268`).

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

- **§4 lax decodes:** brute-force the encoding space per group under
  `qemu-aarch64` user-mode and diff — one pass catches all of these.
- **§2.7:** program a large CNTP_CVAL, read CNTP_TVAL, and check it against the
  signed 32-bit view.
- **§3.x:** dev-tool severity; review-only fixes (add the fw_cfg bound; advertise
  or drop FLUSH; eyeball the 9p readdir/blk used_len paths).
- Regression: `make test` (m1–m15) after each fix. All items above are minor
  (differential-noise / dev-tool / documented-approximation); there are no known
  open correctness bugs in the advertised ISA contract.
