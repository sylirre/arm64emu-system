# ARM64 (AArch64) system emulator.
# C11 + libc/POSIX; networking via bundled libslirp (user-space NAT, no TAP/TUN).

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

# ---- bundled libslirp ----
SLIRP_DIR   := third_party/libslirp-v4.9.3
SLIRP_BUILD := $(SLIRP_DIR)/build
SLIRP_LIB   := $(SLIRP_BUILD)/libslirp.a

# Include paths: src/ for libslirp.h; build/ for generated libslirp-version.h.
CFLAGS  += -I$(SLIRP_DIR)/src -I$(SLIRP_BUILD)
LDLIBS  += $(shell pkg-config --libs glib-2.0)

# ---- emulator sources ----
SRC := $(wildcard src/*.c) $(wildcard src/devices/*.c) $(wildcard src/net/*.c)
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)
BIN := arm64emu

.PHONY: all clean test slirp
all: $(BIN)

# Build libslirp as a static archive (once; meson setup is idempotent).
$(SLIRP_LIB):
	meson setup --default-library=static --buildtype=release $(SLIRP_BUILD) $(SLIRP_DIR)
	ninja -C $(SLIRP_BUILD)

slirp: $(SLIRP_LIB)

# Main binary depends on libslirp; $^ includes both OBJ and SLIRP_LIB.
$(BIN): $(OBJ) $(SLIRP_LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEP)

clean:
	rm -f $(OBJ) $(DEP) $(BIN)
	rm -rf $(SLIRP_BUILD)

test: $(BIN)
	@tests/run_tests.sh
