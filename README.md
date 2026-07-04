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

# Share host directories into the guest over virtio-9p (repeatable):
./arm64emu -bios .../QEMU_EFI.fd -kernel Image -initrd initramfs \
           -virtfs /path/to/project,tag=proj \
           -virtfs /srv/data,tag=data,ro

# virtio-console on hvc0, so the guest terminal size tracks the host window:
./arm64emu -bios .../QEMU_EFI.fd -kernel Image -initrd initramfs \
           -console virtio \
           -append "console=hvc0 earlycon=pl011,0x9000000"
```

Useful flags: `-m <MB>` RAM size, `-dtb FILE` supply a device tree,
`-drive IMG[,ro]` attach a virtio-blk disk (repeatable; `ro` opens the image
read-only and advertises VIRTIO_BLK_F_RO), `-net` user-mode
networking, `-virtfs DIR[,tag=TAG][,ro]` share a host directory over virtio-9p
(repeatable), `-console pl011|virtio` pick the console device (default `pl011`),
`-bin FILE@ADDR` load a flat binary (bare-metal tests), `-d`
per-instruction trace, `-rt` register trace, `-maxinsn N` stop after N
instructions.

**Host directory sharing (`-virtfs`).** Each `-virtfs DIR` exports a host
directory to the guest as a 9P2000.L filesystem on its own virtio-mmio slot.
`tag=` names the mount (default: the directory's basename); `ro` makes it
read-only (default is read-write, changes pass straight through to the host).
Repeat `-virtfs` for multiple independent shares. Mount inside the guest with:

```sh
mount -t 9p -o trans=virtio,version=9p2000.L proj /mnt/proj
```

The guest kernel needs 9p-over-virtio support (`CONFIG_NET_9P`,
`CONFIG_NET_9P_VIRTIO`, `CONFIG_9P_FS`, plus `virtio_mmio`) built in or as
loadable modules. `..` is confined to the share root; symlinks inside a share
are followed by the host, so a share is a convenience, not a security boundary.

**Console (`-console pl011|virtio`).** The default `pl011` is the PL011 UART
(`ttyAMA0`): a byte-pipe serial line, unchanged and fully deterministic.
`-console virtio` additionally attaches a **virtio-console** on its own
virtio-mmio slot, exposed to the guest as `hvc0`. It advertises
`VIRTIO_CONSOLE_F_SIZE`, so the guest terminal size follows the host window
automatically — the initial columns/rows and every later host resize (SIGWINCH)
are propagated, and full-screen programs (`vi`, `less`, `top`) redraw without a
manual `resize`. Boot with `console=hvc0` to move `/dev/console` there (keep
`earlycon=pl011,0x9000000` for early-boot output before the driver probes; the
PL011 stays live as `ttyAMA0`). The guest kernel needs `CONFIG_VIRTIO_CONSOLE`
(with `CONFIG_HVC_DRIVER`) built in or in the initramfs. Until the guest driver
is up — firmware menus, early boot — keyboard input stays on the PL011, so UEFI
menus still work. Note that host resize events make `virtio` mode as
non-deterministic as any keyboard input; the default `pl011` path is untouched.

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
  devices/      gicv2, timer, pl011, pl031, psci, fwcfg, virtio_blk (-drive),
                virtio_net (-net), virtio_9p (-virtfs), virtio_console (-console)
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
  **performance** is ~40 MIPS, so a full distro boot is slow (see *Performance
  notes* below for why a decoded-instruction cache did **not** help).
- **Devices**: **virtio-blk** (`-drive`, disk-backed rootfs), **virtio-net**
  (`-net`, user-mode NAT via libslirp), **virtio-9p** (`-virtfs`, host
  directory sharing), and **virtio-console** (`-console virtio`, host-tracking
  `hvc0` terminal size) are implemented over virtio-mmio, alongside the fw_cfg
  initramfs rootfs.

These are incremental extensions along the path already established. The hard
parts — a correct CPU/MMU/exception core, the device model, the CFI flash and
firmware↔platform contract (DTB, fw_cfg, PSCI), and the full firmware→kernel→
userspace hand-off — are done and validated against QEMU.

## Performance notes

The interpreter runs at **~40 MIPS** (firmware boot prefix, `-O2`, single core).
A pure interpreter spends its time on the per-instruction work itself — fetch,
operand extraction, the ALU/memory operation — not on classifying the opcode.

### Decoded-instruction cache — evaluated, not adopted

A **decoded-instruction cache** (the classic "memoize the decode" interpreter
optimization, *not* a JIT) was implemented and benchmarked, then reverted. It was
a direct-mapped table keyed by PC that cached the resolved group-handler function
pointer for each instruction word, re-validated against the live word on every
hit (so it stayed correct for self-modifying / decompressed code with no flush).

It was **correct** — full CPU state stayed byte-for-byte identical to the plain
interpreter over 50M firmware instructions, at a ~99.99% hit rate — but it **did
not improve throughput; it was ~5–6% slower**. Findings, kept here so the
experiment isn't blindly repeated:

- This decode tree is **shallow and branch-predictable** (a 4-bit top-level
  `switch`, then short per-group chains). The classification a cache hit skips is
  only a handful of correctly-predicted branches — cheaper than what the cache
  adds: an **indirect call** through the cached pointer (mispredicts) plus the
  **data-cache pressure** of a ~1 MB table.
- At ~90 cycles/instruction the hot loop is extremely sensitive: merely *defining*
  the function-pointer classifier inside `decode.c` (taking the handlers'
  addresses) perturbed that translation unit's codegen by ~6%, even when the cache
  was disabled.
- The real lever for a pure interpreter is **operand pre-decode** — caching a
  fully decoded form (opcode id + pre-extracted operands) dispatched via a dense
  `switch`, so a hit skips operand extraction too, not just classification. That,
  or accepting the current speed, is the path forward; a function-pointer decode
  cache is not.
