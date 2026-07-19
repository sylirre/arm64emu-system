#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Sylirre
# Differential trace harness: run a flat binary under QEMU (the oracle) and under
# our emulator, then diff the per-instruction PC stream to find the first
# divergence. Used during M4 firmware bring-up. QEMU is a *development* tool only.
#
# usage: diff_qemu.sh BIN LOAD_ADDR [MAXINSN]
set -u
BIN="${1:?bin}"; LOAD="${2:-0x40000000}"; MAX="${3:-200}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT

# Our emulator trace: "<pc>: <insn> ..."
"$ROOT/arm64emu" --bin "$BIN@$LOAD" --trace --max-insn "$MAX" 2>"$OUT/mine.raw"
grep -oE '^[0-9a-f]{16}: [0-9a-f]{8}' "$OUT/mine.raw" | awk '{print $1}' > "$OUT/mine.pc"

# QEMU trace via -d in_asm,nochain -singlestep. (-kernel loads flat bin at RAM base.)
qemu-system-aarch64 -M virt -cpu cortex-a57 -m 256 -nographic \
    -kernel "$BIN" -d in_asm -singlestep -D "$OUT/qemu.log" \
    -no-reboot >/dev/null 2>&1 &
QPID=$!; sleep 1; kill $QPID 2>/dev/null
grep -oE '^0x[0-9a-f]+:' "$OUT/qemu.log" | tr -d 'x:' | sed 's/^0*//' > "$OUT/qemu.pc" 2>/dev/null

echo "our first PCs:"; head -10 "$OUT/mine.pc"
echo "diff (ours vs qemu):"; diff <(head -"$MAX" "$OUT/mine.pc") <(head -"$MAX" "$OUT/qemu.pc") | head -20
