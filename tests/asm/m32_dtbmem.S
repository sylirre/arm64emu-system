/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M32 test: the device tree's /memory node is sized to --memory.
 *
 * run_tests.sh runs this one as `--bios <img> --memory 384`, not `--bin`: the
 * device tree is placed at the base of RAM only when platform_build runs (under
 * --bios/--kernel). So this executes from flash at 0x0, at EL1 with the MMU off.
 *
 * The built-in tree is compiled from a 1 GB reference machine, and for a long
 * time that was all a guest could ever be told it had — nothing rewrote the
 * blob, so any --memory but 1024 handed the firmware a tree that disagreed with
 * the RAM actually mapped and EDK2 faulted on the first access past the end.
 * fdt_set_memory now patches the node (src/fdt/fdt.c).
 *
 * No FDT parser here: the property value is four big-endian cells
 * <0x0 0x40000000 0x0 0x18000000> (base 1 GB mark, size 384 MB), which as
 * little-endian words read 0, 0x40, 0, 0x18. Scan the first 8 KB of RAM — the
 * whole blob is ~7 KB — for that tuple, and require the unpatched 1 GB tuple
 * (..., 0, 0x40) to be absent, so a stale blob cannot pass this by accident.
 * x0=0 on success, else a failing-check id. */

.text
.global _start
_start:
    movz    x9, #0x4000, lsl #16        /* 0x40000000: RAM base, where the DTB sits */
    add     x10, x9, #0x2000            /* scan limit: 8 KB covers the ~7 KB blob */
    mov     x11, #0                     /* found: patched 384 MB tuple */
    mov     x12, #0                     /* found: unpatched 1 GB tuple */

scan:
    ldp     w0, w1, [x9]                /* cells 0,1: address high, low */
    ldp     w2, w3, [x9, #8]            /* cells 2,3: size high, low */
    add     x9, x9, #4                  /* word-granular scan: the blob is only
                                         * 4-byte aligned as a whole */
    cbnz    w0, next                    /* address-high must be 0 */
    cmp     w1, #0x40                   /* address-low BE 0x40000000 -> LE 0x40 */
    b.ne    next
    cbnz    w2, next                    /* size-high must be 0 */
    cmp     w3, #0x18                   /* size-low BE 0x18000000 -> LE 0x18 */
    b.eq    hit_patched
    cmp     w3, #0x40                   /* the compiled-in 1 GB size */
    b.eq    hit_stale
next:
    cmp     x9, x10
    b.lo    scan
    b       done

hit_patched:
    mov     x11, #1
    b       next
hit_stale:
    mov     x12, #1
    b       next

done:
    cbz     x11, fail1                  /* no /memory reg sized to --memory */
    cbnz    x12, fail2                  /* an unpatched 1 GB tuple survived */

    movz    x0, #0
    hlt     #0
fail1:
    movz    x0, #171
    hlt     #0
fail2:
    movz    x0, #172
    hlt     #0
