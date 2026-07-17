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
# -fno-math-errno lets __builtin_sqrt/fabs inline to hardware FP ops, so the
# scalar-FP interpreter needs no libm (keeping the build libc-only, no -lm).
CFLAGS  ?= $(CSTD) $(OPT) $(WARN) $(DEFS) -Isrc -fno-math-errno -g
LDFLAGS ?=
LDLIBS   =

# ---- emulator sources ----
SRC := $(wildcard src/*.c) $(wildcard src/devices/*.c) $(wildcard src/net/*.c) $(wildcard src/jit/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)
BIN := arm64emu

.PHONY: all clean test
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEP)

clean:
	rm -f $(OBJ) $(DEP) $(BIN) arm64emu-a64

test: $(BIN)
	@tests/run_tests.sh

# JIT suite: the asm tests under -jit, then interpreter-vs-jit consistency
# on a deterministic firmware+Linux boot (byte-identical state checkpoints).
test-jit: $(BIN)
	@EMU_FLAGS=-jit tests/run_tests.sh
	@tests/run_jit_consist.sh

# AArch64-backend validation without ARM hardware: cross-build a static
# aarch64 binary (clang + lld + Debian cross libc) and run the -jit suite
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
	AE_RUNNER=qemu-aarch64 AE_EMU=$(CURDIR)/arm64emu-a64 EMU_FLAGS=-jit tests/run_tests.sh
	AE_RUNNER=qemu-aarch64 AE_EMU=$(CURDIR)/arm64emu-a64 AE_POINTS=4000000 tests/run_jit_consist.sh
