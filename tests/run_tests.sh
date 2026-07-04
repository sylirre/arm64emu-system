#!/usr/bin/env bash
# Build and run the assembly self-tests against the emulator.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMU="$ROOT/arm64emu"
ASM="$ROOT/tests/asm"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# Toolchain: prefer clang (cross by default), else aarch64-linux-gnu-gcc.
CC_AS=""
if command -v clang >/dev/null;             then CC_AS="clang --target=aarch64-elf"; fi
if command -v aarch64-linux-gnu-gcc >/dev/null; then CC_AS="aarch64-linux-gnu-gcc"; fi
OBJCOPY="$(command -v llvm-objcopy || command -v aarch64-linux-gnu-objcopy || command -v objcopy)"

if [ -z "$CC_AS" ] || [ -z "$OBJCOPY" ]; then
    echo "SKIP: no aarch64 assembler/objcopy available"; exit 0
fi

LOAD=0x40000000
pass=0; fail=0
for src in "$ASM"/*.S; do
    name="$(basename "$src" .S)"
    elf="$OUT/$name.elf"; bin="$OUT/$name.bin"
    if ! $CC_AS -nostdlib -Wl,-Ttext=$LOAD -Wl,-e,_start "$src" -o "$elf" 2>"$OUT/err"; then
        echo "FAIL $name (assemble)"; cat "$OUT/err"; fail=$((fail+1)); continue
    fi
    "$OBJCOPY" -O binary "$elf" "$bin"
    # Run; capture the HLT line "x0=0x...". Most tests load as a flat -bin image;
    # the virtio-console probe needs the platform (virtio devices exist only when
    # platform_build runs), so run it as firmware with -console virtio instead.
    case "$name" in
        *_vcon) run=("$EMU" -bios "$bin" -console virtio -maxinsn 100000) ;;
        *)      run=("$EMU" -bin "$bin@$LOAD" -maxinsn 100000) ;;
    esac
    res="$("${run[@]}" 2>&1 | grep -oE 'x0=0x[0-9a-f]+' | head -1)"
    if [ "$res" = "x0=0x0" ]; then
        echo "PASS $name"; pass=$((pass+1))
    else
        echo "FAIL $name ($res)"; fail=$((fail+1))
    fi
done
echo "----"
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
