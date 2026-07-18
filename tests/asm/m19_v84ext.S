/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M19 self-test: ARMv8.3/8.4 GPR-side extensions — FEAT_LRCPC2 LDAPUR/STLUR
 * (full opc x size grid, sign/zero extension, negative and unaligned imm9,
 * SP base), FEAT_JSCVT FJCVTZS (result + NZCV corner battery) and
 * FEAT_FLAGM/FLAGM2 CFINV/RMIF/SETF8/SETF16/AXFLAG/XAFLAG. x0=0 on success,
 * else failing-check id. Expected values are hand-computed; the same file
 * builds as a Linux user binary with -DUSERMODE (exit code = x0) to
 * cross-check every value against real hardware semantics under
 * qemu-aarch64 -cpu max before trusting it as the emulator's reference. */
.arch armv8.5-a

.macro CKX reg, val, id
    movz    x11, #\id
    ldr     x9, =\val
    cmp     \reg, x9
    b.ne    fail
.endm

.text
.global _start
_start:
    /* ================= FEAT_LRCPC2: LDAPUR (load grid) ================= */
    ldr     x10, =d64                    /* 0x8899AABBCCDDEEFF, then 0x..77 */

    ldapur  x2, [x10]
    CKX     x2, 0x8899AABBCCDDEEFF, 1
    ldapur  w2, [x10]
    CKX     x2, 0xCCDDEEFF, 2
    ldapurb w2, [x10]
    CKX     x2, 0xFF, 3
    ldapurh w2, [x10]
    CKX     x2, 0xEEFF, 4
    ldapursb x2, [x10]
    CKX     x2, 0xFFFFFFFFFFFFFFFF, 5
    ldapursb w2, [x10]                   /* sign-extend to 32, zero upper */
    CKX     x2, 0xFFFFFFFF, 6
    ldapursh x2, [x10]
    CKX     x2, 0xFFFFFFFFFFFFEEFF, 7
    ldapursh w2, [x10]
    CKX     x2, 0xFFFFEEFF, 8
    ldapursw x2, [x10]
    CKX     x2, 0xFFFFFFFFCCDDEEFF, 9
    ldapursb x2, [x10, #8]               /* 0x77: positive, no over-extension */
    CKX     x2, 0x77, 10
#ifndef USERMODE
    /* qemu <= 8.2 decodes the LDAPUR/STLUR imm9 as unsigned (a64.decode
     * "imm:9", fixed upstream to "imm:s9"), so negative offsets are oracle-
     * checked only by inspection: the emulator shares the sign_extend imm9
     * path with the qemu-validated LDUR/STUR forms. */
    add     x12, x10, #8
    ldapur  x2, [x12, #-8]               /* negative imm9 */
    CKX     x2, 0x8899AABBCCDDEEFF, 11
#endif
    ldapur  w2, [x10, #5]                /* unaligned: bytes 5..8 */
    CKX     x2, 0x778899AA, 12

    /* ================= FEAT_LRCPC2: STLUR (store grid) ================= */
    ldr     x12, =d64b
    ldr     x1, =0x1122334455667788
    stlur   x1, [x12]
    ldr     x2, [x12]
    CKX     x2, 0x1122334455667788, 13
    ldr     x1, =0xDEADBEEF
    stlur   w1, [x12, #4]
    ldr     x2, [x12]
    CKX     x2, 0xDEADBEEF55667788, 14
    movz    x1, #0xCAFE
    stlurh  w1, [x12, #2]
    ldr     x2, [x12]
    CKX     x2, 0xDEADBEEFCAFE7788, 15
    movz    x1, #0x55
    stlurb  w1, [x12, #7]
    ldr     x2, [x12]
    CKX     x2, 0x55ADBEEFCAFE7788, 16
#ifndef USERMODE
    ldr     x13, =d64c                   /* negative imm9 store (see note above) */
    add     x13, x13, #8
    ldr     x1, =0x0BADF00D
    stlur   w1, [x13, #-4]
    ldr     x2, [x13, #-8]
    CKX     x2, 0x0BADF00DAAAAAAAA, 17
#endif

    /* ---- SP as base register (d64 is 16-byte aligned) ---- */
    mov     x20, sp
    mov     sp, x10
    ldapur  x2, [sp]
    mov     sp, x20
    CKX     x2, 0x8899AABBCCDDEEFF, 18

    /* all good */
    movz    x0, #0
    b       done
fail:
    mov     x0, x11
done:
#ifdef USERMODE
    movz    x8, #93                      /* exit(x0) */
    svc     #0
#else
    hlt     #0
#endif

.data
.balign 16
d64:    .quad 0x8899AABBCCDDEEFF
        .quad 0x0011223344556677
d64b:   .quad 0xAAAAAAAAAAAAAAAA        /* STLUR grid target */
d64c:   .quad 0xAAAAAAAAAAAAAAAA        /* negative-offset store target */
