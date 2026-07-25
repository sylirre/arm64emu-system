# ARM64EMU

Guidelines for AI agents when working with this codebase.

## Overview

This is ARM64 emulator of machine compatible with QEMU 'virt'.

Target platforms are regular Linux distributions (Musl, GNU libc) and Android OS (Bionic, restricted by SELinux and seccomp).

Has both interpreter and JIT execution modes.

## Tooling

Packages required to compile the program and run test suite:

* gcc or clang
* aarch64-linux-gnu-gcc
* make
* qemu-system-aarch64
* qemu-aarch64
* python3
* nc
* cpio
* md5sum
* curl
* Alpine Linux kernel and initramfs (aarch64)

## Project structure

```
Makefile                  build (C11 + libc/POSIX + -lm only, no external deps)
                          and every test target; CC/OPT/CFLAGS overridable
src/
  main.c                  entry point: CLI parsing, --help text (the option and
                          env-var reference), image loading, the run loop,
                          signal-flush of diagnostics, AE* debug env vars
  types.h                 u8..u64 / bool / small helpers
  cpu.h                   CPU state struct, PSTATE bits, core interpreter API
  cpu.c                   reset, cpu_step (fetch/decode/execute driver),
                          condition codes, cpu_dump, and the diagnostics:
                          SVC syscall trace, musl-heap tracker (AEHEAP),
                          AECOV coverage-divergence finder, PC ring buffer,
                          hot-PC profiler
  decode.c                A64 decoder/executor, part 1 — DP-immediate,
                          DP-register, branches/exceptions/system, the whole
                          load/store family, exclusives, LSE atomics,
                          FEAT_MOPS, CRC32, AdvSIMD load/store.
                          exec_a64() is the top-level dispatch and the
                          correctness reference for all three engines
  exec_fpsimd.c           A64 decoder/executor, part 2 — scalar FP, the full
                          Advanced-SIMD surface (three-same/-diff/-extra,
                          by-element, across-lanes, two-misc, shift-imm, copy,
                          permute/TBL/EXT, scalar AdvSIMD), FP16, ARMv8 crypto
                          (AES/PMULL/SHA1/SHA256/SHA512/SHA3), FPSR flag fold
  sysreg.h                MSR/MRS + SMCCC hooks (weak, so M1 links standalone)
  sysreg.c                system-register file: MSR/MRS/SYS/MSR-immediate, ID
                          registers (what features the guest kernel probes),
                          generic-timer registers, DC ZVA, TLBI, AT
  esr.h                   ESR_ELx exception classes + syndrome construction
  exception.c             exception entry/return: VBAR vectors, ESR/FAR/ELR/
                          SPSR banking, DAIF masking, ERET
  mmu.h                   translation + typed guest memory access API,
                          D-TLB layout (shared with the JIT fast path)
  mmu.c                   stage-1 walk (EL1&0, 4 KB granule), software TLB,
                          instruction-fetch cache, guest load/store helpers
  machine.h               Machine struct + virt platform constants
                          (FLASH/RAM base and size, device windows)
  memory.c                physical bus: RAM, Intel CFI NOR flash command set,
                          MMIO dispatch, block read/write
  platform.c              platform wiring: instantiate the virt devices, place
                          the DTB, load kernel/initrd via fw_cfg,
                          machine_reset / machine_tick / WFI wait
  tty.h, tty.c            raw-terminal serial console (termios), window size
  devices.h               device structs + virt MMIO map and INTID constants
  devices/
    gicv2.c               GICv2 distributor + CPU interface (single CPU)
    timer.c               architected generic timer (host clock, or the
                          deterministic instruction-count clock at AE_RTCLOCK=0)
    pl011.c               PL011 UART: TX -> stdout, RX <- stdin, RX/TX IRQs
    pl031.c               PL031 RTC (host wall clock or fixed epoch)
    psci.c                PSCI over the SMC/HVC conduit, incl. SYSTEM_RESET
    fwcfg.c               QEMU fw_cfg: data port + DMA, legacy kernel keys
    virtio_blk.c          --drive, repeatable (disks take slots 1,2,3…)
    virtio_net.c          --net / --netfwd, backed by src/net/ (slot 0)
    virtio_9p.c           --virtfs host directory share (9P2000.L)
    virtio_console.c      --console virtio (hvc0) + host window-size tracking
  fdt/
    virt.dts              device tree source (QEMU virt tree)
    virt_dtb.h            dtc output, embedded in the binary — regenerate,
                          never hand-edit
  jit/                    accelerated engines (predecoded tier + --jit)
    predecode.h           PDEnt: dense opcode id + pre-extracted operands
    predecode.c           the pre-decode classifier (mirrors decode.c; anything
                          uncertain stays PD_GENERIC -> exec_a64) AND
                          pd_run/pd_step, the default computed-goto tier
    jit.h                 public JIT interface (--jit, off by default)
    jit_priv.h            internals shared between runtime and backends
    jit.c                 JIT runtime: code cache, block tables, dispatch loop,
                          SMC coherence, memory slow paths, AEJIT_* knobs
    ir.h                  the IR: op set, virtual registers, block layout
    frontend.c            basic-block discovery + PDEnt -> IR translation
                          (guest-shaped concerns are all resolved here)
    backend_x86_64.c      x86-64 code generator
    backend_a64.c         AArch64 code generator (same-ISA host)
  net/                    usernet: built-in user-mode NAT stack (no libslirp)
    usernet.h             device-facing API (input/poll/output callback)
    usernet_priv.h        internals: fixed topology, shared helpers
    usernet_core.c        Ethernet/ARP/IPv4, ICMP echo, poll driver, pcap, stats
    usernet_udp.c         DHCP server, UDP NAT flow table, DNS redirect
    usernet_tcp.c         terminating TCP proxy over host sockets

tests/
  run_tests.sh            builds and runs the asm suite (EMU_FLAGS picks the
                          engine; AE_EMU/AE_RUNNER pick an alternate binary)
  run_pd_consist.sh       predecoded tier vs interpreter: identical serial
                          output + CPU state at exact --max-insn checkpoints
  run_jit_consist.sh      same comparison for --jit
  run_bootlog_gate.sh     full 1.6B-insn firmware+Linux boot-log diff
                          (mandatory after JIT/pd frontend changes)
  run_fuzz_engines.sh     cross-engine differential fuzzing over random blocks
  asm/                    self-checking tests; x0=0 on success, else check id.
                          Flat --bin images unless the test needs MMIO devices,
                          which only exist under --bios (marked below; the list
                          lives in run_tests.sh's per-name case). "dual-mode"
                          tests also build as a Linux user binary with
                          -DUSERMODE so qemu-aarch64 can vet the expected values
    m1_int.S              integer ISA
    m2_mmu.S              MMU enable, VA access, SVC exception
    m4_fpsimd.S           MOVI/DUP/INS/UMOV/SMOV + vector load/store
    m5_ldsingle.S         AdvSIMD single-structure / replicate load-store
    m6_cow.S              copy-on-write fault handling via data aborts
    m7_crypto.S           REV + SHA-1/SHA-256/AES
    m8_simd.S             AUTO-GENERATED AdvSIMD differential vs qemu
    m9_simd_int.S         AUTO-GENERATED integer AdvSIMD differential
    m10_simd_scalar.S     AUTO-GENERATED scalar AdvSIMD differential
    m11_reboot.S          PSCI SYSTEM_RESET warm reboot (RAM survives)
    m12_vcon.S            virtio-console probe (--bios --console virtio)
    m13_lse.S             FEAT_LSE atomics, CAS/CASP, LDAPR (dual-mode)
    m14_spalign.S         SP-alignment fault (SCTLR_EL1.SA)
    m15_rtc.S             PL031 match/alarm interrupt (--bios)
    m16_laxdecode.S       unallocated encodings must UNDEF
    m17_at_unpriv.S       LDTR/STTR EL0 view + AT S1E*R/W -> PAR_EL1
    m18_fptrap.S          CPACR_EL1.FPEN traps (EC 0x07), EL0 drop
    m19_v84ext.S          LRCPC2 / JSCVT / FLAGM+FLAGM2 (dual-mode)
    m20_ext_simd.S        RDM / DotProd / FCMA / FHM + FRINTX/I (dual-mode)
    m21_mops.S            FEAT_MOPS CPYx/CPYFx/SETx contract (dual-mode)
    m22_fpsr.S            FPSR cumulative flags, QC, FCMP-vs-FCMPE (dual-mode)
    m23_jitpar.S          engine-parity regressions found by the fuzzer
    m24_gic.S             GICv2 distributor + CPU interface (--bios)
    m25_timer.S           generic timer reads, IRQ delivery, WFI (--bios)
    m26_irq.S             IRQ/sync exception entry, banking, ERET (--bios)
    m27_excl.S            exclusive monitor LDXR/STXR/LDXP/STXP/CLREX
    m28_uart.S            PL011 flags, masks, TX IRQ via the GIC (--bios)
    m29_psci.S            PSCI conduit queries over HVC
    m30_sysreg.S          ID-register constants + MSR/MRS round-trip
    m31_mmu2.S            MMU corners: TBI, EPD, TxSZ fault, 4-level walk
  scripts/
    fuzz_gen.c            random-block generator for run_fuzz_engines.sh
    diff_qemu.sh          per-instruction PC-stream diff vs QEMU (dev oracle)
    net_smoke.sh          end-to-end usernet test (dhcp/dns/wget/fwd/upload)
  net/                    guest side of net_smoke.sh
    mk_guest_initrd.sh    build the BusyBox test initramfs from a stock Alpine one
    guest-init            guest /init; runs the phase named by aetest=
    udhcpc.script         minimal DHCP event script
```

## Conventions and rules

General:

* Be thorough in reasoning and concise in output.
* Do not re-read files unless they were changed.
* Do not switch branches, do not look up changes on other branches unless explicitly were asked for this.
* Think about best approach when implementing a requested feature. Ask clarifying questions before making architectural changes and propose solution variants, especially if there are caveats and unintended side effects of requested changes.
* New command line options and environment variables must be added into utility built-in help information.
* Keep in sync the interpreter and JIT parts of emulator where possible.
* Keep in sync the code and documentation (`./docs/`).
* Keep in sync the file structure in architecture section of README.md and structure section of AGENTS.md.
* Ensure that code comments are up-to-date after made changes.
* Compiler warnings or errors must be resolved.
* Do not give up chasing bugs. You know the code better than anyone.
* Run test suite after finishing changes.
* Never make a failing test pass by weakening it. Investigate the root cause of test failure. If the test indeed faulty, ask the project owner first before making changes.
* If all tests passed and there are no already staged files, commit your changes to current branch.
* Never run `git push`.

Commit messages:

* Each commit must consist of header and description.
* The header must follow this format: `scope: brief description of change`.
* The commit body must be detailed and explain why change was necessary, what was the story behind it. If that's a new feature, explain what it does. If that's a bugfix, explain what was the bug and how it was fixed.
* Wrap each line of the commit body at 72 characters.
* Add Co-Authored-By footer with explanation who you are.

## Testing

Basic testing which enough for most cases to ensure no regressions:

* `make test`: asm base suite, predecoded tier disabled
* `make test-pd`: asm suite on the default predecoded tier
* `make test-jit`: asm suite under JIT mode

Extras:

* `make test-pd-full`: the above plus run_bootlog_gate.sh (full 1.6B-insn boot log diff)
* `make test-jit-full`: same as previous but under JIT
* `make fuzz-engines`: 200 random-block seeds across interpreter/pd/jit/jit-slowmem/nofuse/novra
* `make test-jit-a64`: cross-builds a static aarch64 binary (clang+lld) and runs the JIT suite under qemu-aarch64

**Important**: do not run any of tests above in parallel with each other.

Acceptance criteria:

* Test suite pass: interpreter + JIT.
* No crashes.
* No hangups.
