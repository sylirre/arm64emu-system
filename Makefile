# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Sylirre
# ARM64 (AArch64) system emulator.
# C11 + libc/POSIX only; networking via the built-in usernet stack (src/net/,
# user-space NAT over host sockets — no TAP/TUN and no external libraries).

CC      ?= cc
CSTD    ?= -std=c11
OPT     ?= -O2
WARN     = -Wall -Wextra -Wno-unused-parameter
DEFS     = -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
# -fno-math-errno lets __builtin_sqrt/fabs inline to hardware FP ops without
# errno handling. libm (-lm) is linked for __builtin_fma/fmaf: the interpreter's
# FMADD/FMLA use a genuinely fused multiply-add (single rounding, matching
# AArch64), which the compiler turns into a hardware FMA under -march=native and
# otherwise into a libm fma() call. Building with -march=native drops the call.
CFLAGS  ?= $(CSTD) $(OPT) $(WARN) $(DEFS) -Isrc -fno-math-errno -g
LDFLAGS ?=
LDLIBS   = -lm

# ---- emulator sources ----
SRC := $(wildcard src/*.c) $(wildcard src/devices/*.c) $(wildcard src/net/*.c) $(wildcard src/jit/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)
BIN := arm64emu

.PHONY: all clean test
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Build-flag survey (TODO_OPTIMIZATIONS #14, 2026-07-19, gcc 13 x86-64):
# -O3/-flto is ~5% faster for --jit and neutral for the interpreter, but the
# --pd computed-goto dispatch TU regresses ~20% under it, and -flto spellings
# aren't portable across gcc/clang — so the default stays plain -O2 and
# `make OPT="-O3 -flto=8"` is the supported opt-in fast recipe. The pin below
# keeps predecode.c at -O2 in that case (a no-op for the default build).
# -mtune=native measured neutral; PGO not pursued (adds a two-phase build for
# a workload-specific gain).
src/jit/predecode.o: override OPT := -O2

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEP)

clean:
	rm -f $(OBJ) $(DEP) $(BIN) arm64emu-a64

test: $(BIN)
	@tests/run_tests.sh

# JIT suite: the asm tests under --jit, then interpreter-vs-jit consistency
# on a deterministic firmware+Linux boot (byte-identical state checkpoints).
test-jit: $(BIN)
	@EMU_FLAGS=--jit tests/run_tests.sh
	@tests/run_jit_consist.sh

# Same pair for the --pd interpreter tier.
.PHONY: test-pd
test-pd: $(BIN)
	@EMU_FLAGS=--pd tests/run_tests.sh
	@tests/run_pd_consist.sh

# The suites plus the full-boot log gate (docs/jit.md: mandatory for frontend
# changes — the consistency checkpoints all sit inside the UEFI phase).
.PHONY: test-jit-full test-pd-full
test-jit-full: test-jit
	@tests/run_bootlog_gate.sh --jit
test-pd-full: test-pd
	@tests/run_bootlog_gate.sh --pd

# Cross-engine differential fuzzing: random blocks over the JIT's inlined
# surface, interpreter vs --pd vs --jit (+ SLOWMEM/NOFUSE/NOVRA variants),
# byte-identical HLT line and full register dump required per seed.
# AE_SEEDS overrides the seed count (default 200); see docs/parity.md.
.PHONY: fuzz-engines
fuzz-engines: $(BIN)
	@tests/run_fuzz_engines.sh

# AArch64-backend validation without ARM hardware: cross-build a static
# aarch64 binary (clang + lld + Debian cross libc) and run the --jit suite
# plus one consistency checkpoint under qemu-user emulation. Skips politely
# when the cross pieces are absent.
A64_SYSROOT = /usr/aarch64-linux-gnu
A64_CC = clang --target=aarch64-linux-gnu -nostdlibinc -I$(A64_SYSROOT)/include \
         -fuse-ld=lld -static -L$(A64_SYSROOT)/lib -B$(A64_SYSROOT)/lib
.PHONY: test-jit-a64
test-jit-a64:
	@command -v qemu-aarch64 >/dev/null 2>&1 && command -v clang >/dev/null 2>&1 \
	    && [ -e $(A64_SYSROOT)/lib/libc.a ] \
	    || { echo "SKIP: qemu-aarch64 / clang / aarch64 cross libc missing"; exit 0; }
	$(A64_CC) $(CSTD) $(OPT) $(WARN) $(DEFS) -Isrc -fno-math-errno \
	    -o arm64emu-a64 $(SRC) -lm
	AE_RUNNER=qemu-aarch64 AE_EMU=$(CURDIR)/arm64emu-a64 EMU_FLAGS=--jit tests/run_tests.sh
	AE_RUNNER=qemu-aarch64 AE_EMU=$(CURDIR)/arm64emu-a64 AE_POINTS=4000000 tests/run_jit_consist.sh
