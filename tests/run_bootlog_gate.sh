#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Sylirre
# Full-boot log gate (the "mandatory gate" of docs/jit.md): boot the same
# deterministic firmware+Linux image deep past the consistency checkpoints —
# which all sit inside the UEFI phase — under the interpreter and under an
# accelerated engine, strip the printk `[ ts ]` prefixes, and require the
# logs to be otherwise identical. A translation bug in an instruction UEFI
# doesn't lean on sails straight past the checkpoints; a mistyped CNTVCT
# encoding once passed 5/5 of them and was caught only by this comparison.
#
# The final CPU state at the stopping point is diffed as information only:
# tick cadence (timer-IRQ delivery points) may legitimately shift register
# state this deep in a boot (docs/jit.md "Known, documented deviations").
#
# Usage: run_bootlog_gate.sh [--jit|--pd]        (default --jit)
# Env: AE_BIOS, AE_KERNEL, AE_INITRD override the images;
#      AE_MAXINSN overrides the stopping point (default 1600000000);
#      AE_EMU / AE_RUNNER: alternate binary and launcher prefix.
set -u
# The runtime default is the host wall clock (AE_RTCLOCK=1); pin the deterministic
# instruction-count clock so the two engines' logs match past the checkpoints.
# Overridable for debugging.
export AE_RTCLOCK="${AE_RTCLOCK:-0}"
FLAG="${1:---jit}"
case "$FLAG" in
    --jit|--pd) ;;
    *) echo "usage: $0 [--jit|--pd]"; exit 2 ;;
esac
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMU="${AE_EMU:-$ROOT/arm64emu}"
BIOS=${AE_BIOS:-/usr/share/qemu-efi-aarch64/QEMU_EFI.fd}
KERNEL=${AE_KERNEL:-$HOME/Image.gz}
INITRD=${AE_INITRD:-$HOME/initrd}
MAXI=${AE_MAXINSN:-1600000000}

[ -x "$EMU" ] || { echo "build arm64emu first"; exit 1; }
[ -r "$BIOS" ] || { echo "SKIP: no firmware at $BIOS"; exit 0; }
[ -r "$KERNEL" ] && [ -r "$INITRD" ] || { echo "SKIP: no kernel/initrd"; exit 0; }

OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
${AE_RUNNER:-} "$EMU" --bios "$BIOS" --kernel "$KERNEL" --initrd "$INITRD" \
    --append console=ttyAMA0 --max-insn "$MAXI" \
    </dev/null >"$OUT/i.out" 2>"$OUT/i.err" &
${AE_RUNNER:-} "$EMU" $FLAG --bios "$BIOS" --kernel "$KERNEL" --initrd "$INITRD" \
    --append console=ttyAMA0 --max-insn "$MAXI" \
    </dev/null >"$OUT/j.out" 2>"$OUT/j.err" &
wait

# Strip printk timestamp prefixes and embedded clock-derived stamps (the
# kernel audit line carries audit(<secs.ms>:<serial>) read from the timer, so
# its value shifts with tick cadence exactly like the prefixes); anything
# still differing is a real bug.
strip_ts() {
    sed -E -e 's/\[ *[0-9]+\.[0-9]+\]//g' \
           -e 's/audit\([0-9]+\.[0-9]+:([0-9]+)\)/audit(TS:\1)/g' "$1"
}
strip_ts "$OUT/i.out" >"$OUT/i.log"
strip_ts "$OUT/j.out" >"$OUT/j.log"

if cmp -s "$OUT/i.err" "$OUT/j.err"; then
    echo "info: final CPU state byte-identical at maxinsn=$MAXI"
else
    echo "info: final CPU state differs at maxinsn=$MAXI (tick cadence permits this):"
    diff "$OUT/i.err" "$OUT/j.err" | head -8
fi

if cmp -s "$OUT/i.log" "$OUT/j.log"; then
    echo "PASS bootlog gate ($FLAG, maxinsn=$MAXI, timestamp-stripped logs identical)"
else
    echo "FAIL bootlog gate ($FLAG): logs differ beyond printk timestamps"
    diff "$OUT/i.log" "$OUT/j.log" | head -30
    exit 1
fi
