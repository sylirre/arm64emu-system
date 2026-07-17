# `-pd`: direct-threaded interpreter tier

`-pd` is an opt-in acceleration of the **interpreter** path (the portable,
non-JIT engine). It is off by default; the plain `cpu_step` interpreter remains
the default and the correctness reference.

## What it does

Each instruction is classified once into a `PDEnt` (dense opcode id +
pre-extracted operands) by `pd_fill` and cached in `g_pdcache`, a direct-mapped
table indexed by `(pc>>2)`. Execution is a **computed-goto direct-threaded loop**
(`pd_run`/`pd_step` in `src/jit/predecode.c`): each handler ends in a `NEXT` that
bumps `icount`, runs the per-instruction rare-event checks, fetches the next word
via `mem_ifetch`, validates the cache entry, and `goto`s the next handler. The
per-handler dispatch gives each site its own branch-predictor slot.

Only the **decode** (operand extraction, ~32% of interpreter time) is cached.
Fetch/MMU translation still runs every instruction, so `-pd` is fully correct in
system mode. The cache is validated by comparing the cached word to the freshly
fetched word, which makes it self-modifying-code- and address-space-safe
(`pd_fill` is a pure function of the word) — no invalidation hook is needed.

Anything `pd_fill` leaves `PD_GENERIC` runs through `exec_a64` (the `L_GENERIC`
handler), so behaviour is byte-identical to the plain interpreter by
construction — the same coverage contract the JIT frontend relies on.

## Why this works where a per-PC cache didn't

An earlier per-PC cache of the resolved group-handler *function pointer*
(dispatched by indirect call) was 5–6% slower here: the decode tree is shallow,
so a hit skipped only a few well-predicted branches while adding an indirect-call
mispredict and cache pressure, and it still ran the full per-instruction step
loop. `pd_run` differs on every count — computed-goto (not indirect call),
replicated per-handler dispatch, and it never returns to a per-instruction loop.
It lives in its own translation unit so it never perturbs `decode.c`'s codegen.

## Measured

Firmware + Linux boot, 400M instructions, min-of-3, this host:

| mode | time | MIPS | vs interp |
|------|------|------|-----------|
| plain interpreter | 10.97 s | ~36.5 | 1.0× |
| `-pd` | 4.46 s | ~89.7 | **2.46×** |

`-jit` remains far faster (~370 MIPS) where a native backend is available; `-pd`
is the win for the portable path (and hosts without a JIT backend). `-jit` wins
if both `-pd` and `-jit` are given.

## Correctness / determinism

`icount` is exact (every instruction, including fetch-aborted ones). Verified
byte-identical against the plain interpreter at 1M/4M/16M/64M/300M via
`tests/run_pd_consist.sh`, and across the full asm suite under `EMU_FLAGS=-pd`.
Like `-jit`, the only accepted deviation is tick-cadence: timer-IRQ delivery
points can shift printk timestamps deep in a long boot (console *content* stays
identical). Forced off when a per-instruction debug facility is active
(`-d`/trace/watchpoints), which expects one `exec_a64` per step.
