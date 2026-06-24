# arm64emu — an AArch64 system emulator (pure interpreter)

A from-scratch **ARMv8-A (AArch64) system emulator** written in C11 with **no
external dependencies** (libc/POSIX only). It is a **pure interpreter** — no JIT,
no dynamic translation. It runs the **real EDK2 `ArmVirtQemu` UEFI firmware** and
**boots Linux to a userspace shell**, with the **serial console on your terminal**.

The emulated platform is the QEMU `virt` machine (GICv2), the standard,
well-documented target for EDK2 and Linux on AArch64.

## What it does

- Loads and runs the **actual** `/usr/share/qemu-efi-aarch64/QEMU_EFI.fd`
  firmware and runs it through SEC/PEI/DXE/BDS to the **UEFI Interactive Shell**
  (`Shell>`), including the 5→1s boot-countdown driven by the generic timer.
- **Boots a Linux kernel** supplied over fw_cfg: the EDK2 EFI-stub decompresses
  the kernel, exits boot services, and Linux comes up — detecting the CPU, GICv2,
  generic timer, PL011 console, PL031 RTC, PSCI and the PCIe host bridge — then
  mounts the initramfs and runs **`/init` in userspace (EL0)**, reaching an
  interactive **BusyBox shell**.

```sh
make                       # builds ./arm64emu  (C11, libc only)
make test                  # runs the assembly self-tests

# Boot the EDK2 firmware to the UEFI shell (serial -> your terminal):
./arm64emu -bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd

# Boot Linux via fw_cfg (the EDK2 -kernel path, no disk needed):
./arm64emu -bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
           -kernel Image -initrd initramfs \
           -append "console=ttyAMA0 earlycon=pl011,0x9000000"
```

Useful flags: `-m <MB>` RAM size, `-dtb FILE` supply a device tree,
`-bin FILE@ADDR` load a flat binary (bare-metal tests), `-d` per-instruction
trace, `-rt` register trace, `-maxinsn N` stop after N instructions.

Debug/bring-up env vars (all off by default, no runtime cost when unset):
`AEDBG=N` device/IRQ + fw_cfg/flash logging, `AEPROF=1` hot-PC profiler,
`AERING=1` recent-instruction ring buffer, `AETPC=0xADDR` dump CPU state at a PC,
`AEWATCH=0xADDR` watch writes to an address, `AEIABORT=1` log instruction aborts,
`AECOV=file` coverage-divergence finder (see below).

## Architecture

```
src/
  main.c        CLI, image loading, the run loop, signal-flush of diagnostics
  cpu.{h,c}     CPU state, fetch/decode/execute driver, condition codes, dump,
                hot-PC profiler / ring buffer / coverage-divergence finder
  decode.c      A64 decoder: integer/branch/load-store, CRC32, AdvSIMD ld/st
  exec_fpsimd.c FP/Advanced-SIMD execution (scalar FP + vector integer)
  sysreg.c      MSR/MRS, ID registers, generic-timer registers, DC ZVA, TLBI
  mmu.c         AArch64 stage-1 walk (4 KB granule) + software TLB + fetch cache
  exception.c   exception entry/return (VBAR vectors, ESR/FAR/ELR/SPSR, ERET)
  memory.c      physical bus: RAM + NOR-flash CFI command set + MMIO dispatch
  tty.c         raw-terminal serial console via termios
  devices/      gicv2, timer, pl011, pl031, psci, fwcfg
  fdt/virt.dts  device tree (QEMU virt tree; compiled to virt_dtb.h, embedded)
tests/          self-checking assembly tests + QEMU differential helpers
```

### What is implemented

- **Integer ISA**: data-processing (immediate & register), shifts/bitfields,
  conditional select/compare, mul/div/madd/smulh/umulh, rev/rbit/clz/cls,
  **CRC32/CRC32C**, branches, the full load/store family incl. LDP/STP,
  pre/post/unscaled/register-offset, **STTR/LDTR** (unprivileged),
  **STNP/LDNP** (non-temporal pair), exclusives and acquire/release.
- **System level**: EL0/EL1, MSR/MRS with a large system-register file, ID
  registers matched to QEMU's cortex-a57, MMU (48-bit VA, 4 KB granule, blocks &
  pages, AP/XN/AF) with a software TLB and an instruction-fetch fast path,
  exception entry/return, SVC/HVC/SMC/BRK, barriers/CLREX/WFI/WFE, TLBI, DC ZVA.
- **FP/SIMD**: scalar floating-point (convert int↔fp, FMOV, FADD/FSUB/FMUL/FDIV,
  FABS/FNEG/FSQRT, FCMP/FCCMP, FCSEL, FCVT, FRINT, FMADD/FMSUB/FNMADD/FNMSUB);
  Advanced-SIMD load/store multiple structures (LD1–LD4/ST1–ST4), modified
  immediate (MOVI/MVNI/…), copy (DUP/INS/UMOV/SMOV), three-same vector integer
  (ADD/SUB/logical/compare/min-max/MUL/ADDP), across-lanes reductions
  (ADDV/UMAXV/UMINV/…), two-register misc (NOT/NEG/ABS/CNT/compare-with-zero),
  and shift-by-immediate (SHL/SSHR/USHR/SSHLL/USHLL).
- **Platform devices**: GICv2, generic timer (periodic IRQs), PL011 serial,
  PL031 RTC, PSCI (HVC/SMC), fw_cfg (data port + DMA, legacy kernel keys),
  **Intel CFI (pflash_cfi01) NOR flash** so EDK2's UEFI variable store works,
  and empty virtio-mmio / PCIe / GICv2m / GPIO transports so probing behaves.

## Differential testing against QEMU (development only)

QEMU is used purely as a *development oracle* — never linked or required to run
`arm64emu`. Two techniques made deep bring-up tractable:

1. **Same-run DTB + coverage.** Boot the same firmware in QEMU, dump its runtime
   device tree from guest memory, and capture `-d in_asm` coverage from the *same
   run* (so the DTB, including its random seed, matches). Reconstruct the full
   set of executed instruction addresses from the `OBJD-T` bytes.
2. **Coverage-divergence finder** (`AECOV=qemu_cov.txt`): run `arm64emu` with the
   identical DTB and stop at the first PC it executes that QEMU never does — i.e.
   the exact point where control flow first leaves QEMU's path. This pinpointed
   each bug (a missing device register, a wrong DTB node, an unimplemented
   instruction) instead of debugging a silent hang.

This located, among others: the virtio-mmio magic value, a too-small device
tree (missing the `cfi-flash`/PCIe/virtio nodes), the CFI flash command set, and
an exception-return bug (instruction-abort ELR pointing at the wrong PC).

## Current state and remaining work

- **Firmware**: boots SEC→PEI→DXE→BDS to the **UEFI Interactive Shell**, with a
  working CFI-flash variable store.
- **Linux**: the EFI-stub kernel boots to **userspace** — kernel init completes,
  `/init` runs, and a **BusyBox shell** is reached over the serial console.
- **Remaining work**: a small tail of Advanced-SIMD opcodes is still grown on
  demand (the kernel/musl exercise more NEON than the firmware); a subtle
  correctness issue can surface deep in a complex init script. Interpreter
  **performance** is ~40 MIPS, so a full distro boot is slow — a decoded-
  instruction cache (still not a JIT) would speed it up. **virtio-blk** (for a
  disk-backed rootfs) is future work; today the rootfs is the fw_cfg initramfs.

These are incremental extensions along the path already established. The hard
parts — a correct CPU/MMU/exception core, the device model, the CFI flash and
firmware↔platform contract (DTB, fw_cfg, PSCI), and the full firmware→kernel→
userspace hand-off — are done and validated against QEMU.
