# ARM64 (AArch64) system emulator — pure interpreter, no external deps.
# C11 + libc/POSIX only.

CC      ?= cc
CSTD    ?= -std=c11
OPT     ?= -O2
WARN     = -Wall -Wextra -Wno-unused-parameter
DEFS     = -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS  ?= $(CSTD) $(OPT) $(WARN) $(DEFS) -g
LDFLAGS ?=
LDLIBS   =

SRC := $(wildcard src/*.c) $(wildcard src/devices/*.c)
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
