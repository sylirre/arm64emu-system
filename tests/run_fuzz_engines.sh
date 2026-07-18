#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Sylirre
# Cross-engine differential fuzzing: random instruction blocks over the JIT's
# inlined surface (tests/scripts/fuzz_gen.c), each image run under the
# interpreter, -pd, -jit, and -jit with the memory/fusing/V-cache machinery
# individually disabled. The blocks are fault-free and device-free, so every
# configuration must produce byte-identical output (serial + cpu_dump +
# HLT line). A run that never reaches HLT (maxinsn hit) is a generator bug.
#
# Env: AE_SEEDS (default 200), AE_NINSNS (0 = seed-derived size sweep),
#      AE_EMU / AE_RUNNER as in run_tests.sh, CC for the generator build,
#      AE_FUZZ_KEEP=dir to keep failing images (default tests/fuzz_failures).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMU="${AE_EMU:-$ROOT/arm64emu}"
SEEDS=${AE_SEEDS:-200}
NINSNS=${AE_NINSNS:-0}
KEEP="${AE_FUZZ_KEEP:-$ROOT/tests/fuzz_failures}"
[ -x "$EMU" ] || { echo "build arm64emu first"; exit 1; }

OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=c11 -O2 -Wall -Wextra -o "$OUT/fuzz_gen" \
    "$ROOT/tests/scripts/fuzz_gen.c" || exit 1

CFGS="pd jit jit_slowmem jit_nofuse jit_novra"
run_cfg() { # $1=cfg $2=bin $3=outprefix
    local flags="" envs=""
    case "$1" in
        interp)      ;;
        pd)          flags=-pd ;;
        jit)         flags=-jit ;;
        jit_slowmem) flags=-jit; envs="AEJIT_SLOWMEM=1" ;;
        jit_nofuse)  flags=-jit; envs="AEJIT_NOFUSE=1" ;;
        jit_novra)   flags=-jit; envs="AEJIT_NOVRA=1" ;;
    esac
    env $envs ${AE_RUNNER:-} "$EMU" $flags -bin "$2@0x40000000" \
        -maxinsn "${MAXI_OVERRIDE:-200000}" \
        </dev/null >"$3.out" 2>"$3.err"
}

pass=0; fail=0; genbug=0
for seed in $(seq 1 "$SEEDS"); do
    BIN="$OUT/f$seed.bin"
    "$OUT/fuzz_gen" "$seed" "$NINSNS" >"$BIN" || { echo "GENBUG seed=$seed (generator failed)"; genbug=$((genbug+1)); continue; }
    run_cfg interp "$BIN" "$OUT/i"
    if grep -q 'maxinsn reached' "$OUT/i.err"; then
        echo "GENBUG seed=$seed (no HLT under interpreter — bad encoding?)"
        mkdir -p "$KEEP"; cp "$BIN" "$KEEP/"; genbug=$((genbug+1)); continue
    fi
    bad=""
    for cfg in $CFGS; do
        run_cfg "$cfg" "$BIN" "$OUT/j"
        cmp -s "$OUT/i.out" "$OUT/j.out" && cmp -s "$OUT/i.err" "$OUT/j.err" \
            || bad="$bad $cfg"
    done
    # Phase 2: -bin HLT stops print no cpu_dump, so the HLT line above only
    # compares x0/icount. Stop every config at the exact pre-HLT icount via
    # -maxinsn (instruction-exact in all engines) and compare the full
    # register dump. The accelerated engines' interpret-tail covers the last
    # ~256 insns, so this phase checks all GPRs/pc/nzcv while phase 1 keeps
    # the tail natively executed.
    I=$(grep -oE 'icount=[0-9]+' "$OUT/i.err" | head -1 | cut -d= -f2)
    if [ -n "$I" ] && [ "$I" -gt 0 ]; then
        MAXI_OVERRIDE=$I run_cfg interp "$BIN" "$OUT/mi"
        for cfg in $CFGS; do
            MAXI_OVERRIDE=$I run_cfg "$cfg" "$BIN" "$OUT/mj"
            cmp -s "$OUT/mi.out" "$OUT/mj.out" && cmp -s "$OUT/mi.err" "$OUT/mj.err" \
                || case " $bad " in *" $cfg "*) ;; *) bad="$bad $cfg(dump)";; esac
        done
    fi
    if [ -n "$bad" ]; then
        echo "FAIL seed=$seed diverging:$bad"
        mkdir -p "$KEEP"; cp "$BIN" "$KEEP/"
        for cfg in $bad; do
            base=${cfg%(dump)}
            if [ "$base" = "$cfg" ]; then
                run_cfg "$base" "$BIN" "$OUT/j"
                echo "--- interpreter vs $cfg (HLT-line diff, head) ---"
                diff "$OUT/i.err" "$OUT/j.err" | head -8
            else
                MAXI_OVERRIDE=$I run_cfg "$base" "$BIN" "$OUT/mj"
                echo "--- interpreter vs $cfg (full-dump diff, head) ---"
                diff "$OUT/mi.err" "$OUT/mj.err" | head -8
            fi
        done
        fail=$((fail+1))
    else
        pass=$((pass+1))
    fi
done
echo "----"
echo "passed=$pass failed=$fail genbugs=$genbug (seeds=$SEEDS)"
[ "$fail" -eq 0 ] && [ "$genbug" -eq 0 ]
