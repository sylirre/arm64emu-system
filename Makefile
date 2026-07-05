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
CFLAGS  ?= $(CSTD) $(OPT) $(WARN) $(DEFS) -fno-math-errno -g
LDFLAGS ?=
LDLIBS   =

# ---- emulator sources ----
SRC := $(wildcard src/*.c) $(wildcard src/devices/*.c) $(wildcard src/net/*.c)
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
	rm -f $(OBJ) $(DEP) $(BIN)

test: $(BIN)
	@tests/run_tests.sh
