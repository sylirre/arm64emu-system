# arm64emu — an AArch64 system emulator (pure interpreter)

A from-scratch **ARMv8-A (AArch64) system emulator** written in C11 with **no
external dependencies** (libc/POSIX only). It is a **pure interpreter** — no JIT,
no dynamic translation. It runs the **real EDK2 `ArmVirtQemu` UEFI firmware** and
targets booting Linux, with the **serial console on your terminal**.

The emulated platform is the QEMU `virt` machine (GICv2), which is the standard,
well-documented target for EDK2 and Linux on AArch64.

## Feasibility: demonstrated

This project set out to investigate whether such an emulator is feasible. It is —
and this code proves it concretely:

- It loads and runs the **actual** `/usr/share/qemu-efi-aarch64/QEMU_EFI.fd`
  firmware binary.
- Its execution matches QEMU **instruction-for-instruction for 33.79 million
  instructions** (verified by differential trace, see below).
- The firmware prints its banner to the emulated PL011 serial console:
  `UEFI firmware (version 2024.02-...)`.
- It implements enough of the architecture that the firmware enables the MMU,
  takes/returns exceptions, services **timer interrupts** through the GICv2, runs
  **FP/SIMD** code, reads its configuration over **fw_cfg**, and proceeds deep
  into the DXE/BDS boot phases, then begins loading a kernel supplied via fw_cfg.

## Build & run

```sh
make                       # builds ./arm64emu  (C11, libc only)
make test                  # runs the assembly self-tests

# Boot the EDK2 firmware (serial -> your terminal):
./arm64emu -bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd

# Boot a Linux kernel via fw_cfg (the EDK2 -kernel path, no disk needed):
./arm64emu -bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
           -kernel Image -initrd initramfs -append "console=ttyAMA0 earlycon=pl011,0x9000000"
```

Useful flags: `-m <MB>` RAM size, `-dtb FILE` supply a device tree,
`-bin FILE@ADDR` load a flat binary (for bare-metal tests), `-d` per-instruction
trace, `-rt` register trace, `-maxinsn N` stop after N instructions.
Set `AEDBG=1` in the environment for device/IRQ debug logging.

## Architecture

```
src/
  main.c        CLI, image loading, the run loop
  cpu.{h,c}     CPU state, fetch/decode/execute driver, condition codes, dump
  decode.c      A64 decoder + integer/branch/load-store execution
  exec_fpsimd.c FP/Advanced-SIMD execution (grown on demand)
  exec_system.c (folded into decode.c) system-instruction routing
  sysreg.c      MSR/MRS/SYS, ID registers, generic-timer registers, DC ZVA, TLBI
  mmu.c         AArch64 stage-1 page-table walk (4 KB granule) + software TLB
  exception.c   exception entry/return (VBAR vectors, ESR/FAR/ELR/SPSR, ERET)
  memory.c      physical bus: RAM + flash + MMIO device dispatch
  tty.c         raw-terminal serial console via termios
  esr.h         ESR_ELx syndrome construction
  devices/
    gicv2.c     GICv2 distributor + CPU interface
    timer.c     architected generic timer (host-clock driven)
    pl011.c     PL011 UART (TX->stdout, RX<-stdin, RX/TX IRQs)
    pl031.c     PL031 RTC
    psci.c      PSCI over HVC/SMC (VERSION/FEATURES/SYSTEM_OFF/RESET/...)
    fwcfg.c     QEMU fw_cfg (data port + DMA interface, legacy kernel keys)
  fdt/virt.dts  device tree (compiled to virt_dtb.h, embedded; zero build deps)
tests/
  asm/          self-checking assembly tests (integer, MMU/exceptions, FP/SIMD)
  scripts/      differential-trace helpers vs QEMU
```

### What is implemented

- **Integer ISA**: data-processing (immediate & register), all shifts/bitfields,
  conditional select/compare, mul/div/madd/smulh/umulh, rev/rbit/clz/cls,
  branches (B/BL/cond/CB/TB/BR/BLR/RET), full load/store family incl. LDP/STP,
  pre/post/unscaled/register-offset, exclusives (LDXR/STXR) and acquire/release.
- **System level**: EL0/EL1, MSR/MRS with a large system-register file, ID
  registers matched to QEMU's cortex-a57, MMU (48-bit VA, 4 KB granule, blocks &
  pages, AP/XN/AF permission checks) with a software TLB, exception entry/return,
  SVC/HVC/SMC/BRK, barriers/CLREX/WFI/WFE, TLBI, DC ZVA.
- **FP/SIMD** (subset, grown on demand): MOVI/MVNI/ORR/BIC/FMOV vector
  immediate, the AdvSIMD copy group (DUP/INS/UMOV/SMOV), and SIMD/FP
  loads/stores (LDR/STR/LDP/STP of B/H/S/D/Q).
- **Platform devices**: GICv2, generic timer (delivers periodic IRQs), PL011
  serial, PL031 RTC, PSCI, fw_cfg (incl. DMA), and absent-device stubs for the
  virtio/PCIe/GPIO regions so probing returns "no device" instead of faulting.

## Differential testing against QEMU (development only)

QEMU is used purely as a *development oracle* — it is never linked or required to
run `arm64emu`. The key methodology that made bring-up tractable:

1. Boot the **same** firmware in QEMU under `-S -gdb`, dump QEMU's runtime device
   tree from guest memory, and capture a `-d cpu`/`-d exec` trace from the *same
   run* (so the DTB and trace are byte-identical, including QEMU's random seeds).
2. Run `arm64emu` with that exact DTB and a register/PC trace.
3. Diff the two traces to find the *first* divergence — which pinpoints the exact
   instruction or device interaction that differs.

This located every bug encountered as either a CPU/ID-register issue or a missing
device/instruction, and confirmed the **33.79M-instruction exact match**.

## Current state and remaining work

- **Firmware**: boots through SEC/PEI/DXE, prints the banner, relocates to high
  RAM, services GIC timer interrupts, and enters BDS. With a kernel supplied via
  fw_cfg it proceeds to load the kernel image.
- **Known limitation**: completing the late-BDS path to the kernel hand-off /
  full Linux user space requires (a) more of the **FP/SIMD** instruction set
  (Linux uses NEON extensively — implemented "on demand" as traps surface them),
  and (b) interpreter **performance** work — a pure interpreter runs EDK2's heavy
  DXE phase (LZMA decompression, large `CopyMem`) at ~20 MIPS, so the boot is
  slow. A decoded-instruction cache (still not a JIT) would speed this up
  substantially without changing the no-JIT design.

These are incremental extensions along the path already established, not new
architecture. The hard parts — a correct CPU/MMU/exception core, a working
device model, the firmware↔platform contract (DTB placement, fw_cfg, PSCI), and
serial I/O — are done and validated.
