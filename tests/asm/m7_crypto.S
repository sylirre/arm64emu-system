/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* M7 crypto self-test: REV16/32/64 + ARMv8 SHA-1/SHA-256/AES extensions.
 * x0=0 on success, else failing-check id. Reference values are independent
 * (Python hashlib / FIPS-197 reference). */
.arch armv8-a+crypto

.macro CKX reg, val, id
    movz    x11, #\id
    ldr     x9, =\val
    cmp     \reg, x9
    b.ne    fail
.endm

.text
.global _start
_start:
    adr     x10, bytes0_15
    ld1     {v0.16b}, [x10]             /* v0 = bytes 0x00..0x0f */

    /* REV32 v1.16b, v0.16b */
    rev32   v1.16b, v0.16b
    umov    x0, v1.d[0]
    CKX     x0, 0x0405060700010203, 1
    umov    x0, v1.d[1]
    CKX     x0, 0x0c0d0e0f08090a0b, 2

    /* REV16 */
    rev16   v2.16b, v0.16b
    umov    x0, v2.d[0]
    CKX     x0, 0x0607040502030001, 3
    umov    x0, v2.d[1]
    CKX     x0, 0x0e0f0c0d0a0b0809, 4

    /* REV64 */
    rev64   v3.16b, v0.16b
    umov    x0, v3.d[0]
    CKX     x0, 0x0001020304050607, 5
    umov    x0, v3.d[1]
    CKX     x0, 0x08090a0b0c0d0e0f, 6

    /* SHA1H Sd,Sn = ROL(Sn,30): ROL(1,30) = 0x40000000 */
    movz    w5, #1
    fmov    s0, w5
    sha1h   s1, s0
    umov    w0, v1.s[0]
    CKX     x0, 0x40000000, 7

    /* SHA256SU0 v0,v1 */
    adr     x10, su0_x
    ld1     {v0.4s}, [x10]
    adr     x10, su0_y
    ld1     {v1.4s}, [x10]
    sha256su0 v0.4s, v1.4s
    umov    x0, v0.d[0]
    CKX     x0, 0xceeeeeeed9999999, 8
    umov    x0, v0.d[1]
    CKX     x0, 0x39999999c4444444, 9

    /* AESE v0,v1 with key=0: SubBytes(ShiftRows(state)) */
    adr     x10, bytes0_15
    ld1     {v0.16b}, [x10]
    movi    v1.16b, #0
    aese    v0.16b, v1.16b
    umov    x0, v0.d[0]
    CKX     x0, 0x7bab01f276676b63, 10
    umov    x0, v0.d[1]
    CKX     x0, 0x2b6f7cfec577d730, 11

    /* AESMC v0,v1 with v1 = bytes 0..15 */
    adr     x10, bytes0_15
    ld1     {v1.16b}, [x10]
    aesmc   v0.16b, v1.16b
    umov    x0, v0.d[0]
    CKX     x0, 0x0104030605000702, 12
    umov    x0, v0.d[1]
    CKX     x0, 0x090c0b0e0d080f0a, 13

    /* ---- Full SHA-256 single-block compression of "abc" ---- */
    adr     x3, sha_iv
    ld1     {v2.4s, v3.4s}, [x3]        /* v2=abcd_init, v3=efgh_init */
    mov     v0.16b, v2.16b
    mov     v1.16b, v3.16b
    adr     x2, sha_msg
    ld1     {v4.4s, v5.4s, v6.4s, v7.4s}, [x2]
    rev32   v4.16b, v4.16b
    rev32   v5.16b, v5.16b
    rev32   v6.16b, v6.16b
    rev32   v7.16b, v7.16b
    adr     x1, sha_k
    movz    w4, #0                      /* round-group counter g = 0..15 */
.Lround:
    ld1     {v16.4s}, [x1], #16         /* K[4g..4g+3] */
    add     v16.4s, v16.4s, v4.4s       /* T = S0 + K */
    mov     v17.16b, v0.16b             /* tmp = abcd */
    sha256h  q0, q1, v16.4s             /* abcd = hash(abcd,efgh,T) */
    sha256h2 q1, q17, v16.4s            /* efgh = hash2(...) */
    cmp     w4, #12
    b.ge    .Lnosched
    mov     v18.16b, v4.16b
    sha256su0 v18.4s, v5.4s             /* su0(S0,S1) */
    sha256su1 v18.4s, v6.4s, v7.4s      /* su1(.,S2,S3) -> next W group */
    mov     v4.16b, v5.16b              /* rotate S0<-S1<-S2<-S3<-new */
    mov     v5.16b, v6.16b
    mov     v6.16b, v7.16b
    mov     v7.16b, v18.16b
    b       .Lcont
.Lnosched:
    mov     v4.16b, v5.16b             /* rotate without producing new schedule */
    mov     v5.16b, v6.16b
    mov     v6.16b, v7.16b
.Lcont:
    add     w4, w4, #1
    cmp     w4, #16
    b.lt    .Lround

    add     v0.4s, v0.4s, v2.4s         /* abcd += IV_lo */
    add     v1.4s, v1.4s, v3.4s         /* efgh += IV_hi */

    umov    w0, v0.s[0]
    CKX     x0, 0xba7816bf, 14
    umov    w0, v0.s[1]
    CKX     x0, 0x8f01cfea, 15
    umov    w0, v0.s[2]
    CKX     x0, 0x414140de, 16
    umov    w0, v0.s[3]
    CKX     x0, 0x5dae2223, 17
    umov    w0, v1.s[0]
    CKX     x0, 0xb00361a3, 18
    umov    w0, v1.s[1]
    CKX     x0, 0x96177a9c, 19
    umov    w0, v1.s[2]
    CKX     x0, 0xb410ff61, 20
    umov    w0, v1.s[3]
    CKX     x0, 0xf20015ad, 21

    /* PMULL Vd.1Q, Vn.1D, Vm.1D (64x64 carryless multiply) */
    adr     x10, pmull_a
    ld1     {v0.1d}, [x10]
    adr     x10, pmull_b
    ld1     {v3.1d}, [x10]
    pmull   v1.1q, v0.1d, v3.1d
    umov    x0, v1.d[0]
    CKX     x0, 0x40a0789828c810f0, 22
    umov    x0, v1.d[1]
    CKX     x0, 0x00e038d8688850b0, 23

    /* PMULL Vd.8H, Vn.8B, Vm.8B: each lane = clmul8(0x53,0xca) = 0x3f7e */
    movi    v0.8b, #0x53
    movi    v3.8b, #0xca
    pmull   v1.8h, v0.8b, v3.8b
    umov    x0, v1.d[0]
    CKX     x0, 0x3f7e3f7e3f7e3f7e, 24
    umov    x0, v1.d[1]
    CKX     x0, 0x3f7e3f7e3f7e3f7e, 25

    /* TBL V0.16B, {V1.16B}, V2.16B : out-of-range index -> 0 */
    adr     x10, tbl_table
    ld1     {v1.16b}, [x10]
    adr     x10, tbl_index
    ld1     {v2.16b}, [x10]
    tbl     v0.16b, {v1.16b}, v2.16b
    umov    x0, v0.d[0]
    CKX     x0, 0x1e1c1a1816141210, 26
    umov    x0, v0.d[1]
    CKX     x0, 0x1b19171500131100, 27

    /* TBX V0.16B, {V1.16B}, V2.16B : out-of-range index keeps old Vd (0xee) */
    movi    v0.16b, #0xee
    tbx     v0.16b, {v1.16b}, v2.16b
    umov    x0, v0.d[0]
    CKX     x0, 0x1e1c1a1816141210, 28
    umov    x0, v0.d[1]
    CKX     x0, 0x1b191715ee1311ee, 29

    /* Permute: ZIP1/ZIP2/UZP1/TRN1 on v1=0x00..0f, v2=0x10..1f */
    adr     x10, tbl_table             /* 0x10..0x1f */
    ld1     {v2.16b}, [x10]
    adr     x10, bytes0_15             /* 0x00..0x0f */
    ld1     {v1.16b}, [x10]
    zip1    v0.16b, v1.16b, v2.16b
    umov    x0, v0.d[0]
    CKX     x0, 0x1303120211011000, 30
    umov    x0, v0.d[1]
    CKX     x0, 0x1707160615051404, 31
    zip2    v0.16b, v1.16b, v2.16b
    umov    x0, v0.d[0]
    CKX     x0, 0x1b0b1a0a19091808, 32
    umov    x0, v0.d[1]
    CKX     x0, 0x1f0f1e0e1d0d1c0c, 33
    uzp1    v0.16b, v1.16b, v2.16b
    umov    x0, v0.d[0]
    CKX     x0, 0x0e0c0a0806040200, 34
    umov    x0, v0.d[1]
    CKX     x0, 0x1e1c1a1816141210, 35
    trn1    v0.16b, v1.16b, v2.16b
    umov    x0, v0.d[0]
    CKX     x0, 0x1606140412021000, 36
    umov    x0, v0.d[1]
    CKX     x0, 0x1e0e1c0c1a0a1808, 37

    /* Pairwise ADDP/SMAXP/UMAXP/UMINP on 4S vectors */
    adr     x10, pw_n
    ld1     {v1.4s}, [x10]
    adr     x10, pw_m
    ld1     {v2.4s}, [x10]
    addp    v0.4s, v1.4s, v2.4s
    umov    x0, v0.d[0]
    CKX     x0, 0x0000000000000003, 38
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000e00000005, 39
    smaxp   v0.4s, v1.4s, v2.4s
    umov    x0, v0.d[0]
    CKX     x0, 0x0000000300000005, 40
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000800000009, 41
    umaxp   v0.4s, v1.4s, v2.4s
    umov    x0, v0.d[0]
    CKX     x0, 0xfffffffdfffffffe, 42
    umov    x0, v0.d[1]
    CKX     x0, 0x00000008fffffffc, 43
    uminp   v0.4s, v1.4s, v2.4s
    umov    x0, v0.d[0]
    CKX     x0, 0x0000000300000005, 44
    umov    x0, v0.d[1]
    CKX     x0, 0x0000000600000009, 45

    /* FCVT D,S (single->double): 1.5f -> 1.5 */
    movz    w5, #0x3fc0, lsl #16        /* 1.5f = 0x3fc00000 */
    fmov    s0, w5
    fcvt    d1, s0
    fmov    x0, d1
    CKX     x0, 0x3ff8000000000000, 46
    /* FCVT S,D (double->single): 2.5 -> 2.5f */
    ldr     x9, =0x4004000000000000     /* 2.5 double */
    fmov    d2, x9
    fcvt    s3, d2
    fmov    w0, s3
    CKX     x0, 0x40200000, 47

    /* AdvSIMD scalar copy (DUP element -> scalar): MOV Dd,Vn.D[1] / MOV Sd,Vn.S[2] */
    adr     x10, bytes0_15
    ld1     {v4.16b}, [x10]            /* v4.d[0]=..0100, v4.d[1]=0f..08 */
    mov     d8, v4.d[1]               /* 0x5e180488: high 64 -> scalar, zero rest */
    umov    x0, v8.d[0]
    CKX     x0, 0x0f0e0d0c0b0a0908, 48
    umov    x0, v8.d[1]
    CKX     x0, 0x0000000000000000, 49
    mov     s9, v4.s[2]               /* bytes 8..11 -> scalar */
    umov    w0, v9.s[0]
    CKX     x0, 0x0b0a0908, 50

    /* EXT with distinct Vn/Vm (concat is Vm:Vn; Vn=low). v1=00..0f, v2=10..1f */
    adr     x10, bytes0_15
    ld1     {v1.16b}, [x10]
    adr     x10, tbl_table            /* 0x10..0x1f */
    ld1     {v2.16b}, [x10]
    ext     v0.16b, v1.16b, v2.16b, #8
    umov    x0, v0.d[0]
    CKX     x0, 0x0f0e0d0c0b0a0908, 51
    umov    x0, v0.d[1]
    CKX     x0, 0x1716151413121110, 52
    ext     v0.16b, v1.16b, v2.16b, #1
    umov    x0, v0.d[0]
    CKX     x0, 0x0807060504030201, 53
    umov    x0, v0.d[1]
    CKX     x0, 0x100f0e0d0c0b0a09, 54

    /* AdvSIMD multiply-by-element (vector x indexed). vn={2,-2,4,5}.4s, vm.s[2]=5 */
    adr     x10, mbe_vn
    ld1     {v14.4s}, [x10]
    adr     x10, mbe_vm
    ld1     {v7.4s}, [x10]
    umull   v0.2d, v14.2s, v7.s[2]     /* 0x2f87a9d7 form: {2*5, 0xfffffffe*5} */
    umov    x0, v0.d[0]
    CKX     x0, 0x0a, 55
    umov    x0, v0.d[1]
    CKX     x0, 0x4fffffff6, 56
    smull   v0.2d, v14.2s, v7.s[2]     /* signed: d[1] = -2*5 = -10 */
    umov    x0, v0.d[1]
    CKX     x0, 0xfffffffffffffff6, 57
    umull2  v0.2d, v14.4s, v7.s[2]     /* high half {4*5, 5*5} */
    umov    x0, v0.d[0]
    CKX     x0, 0x14, 58
    umov    x0, v0.d[1]
    CKX     x0, 0x19, 59
    adr     x10, mbe_accd
    ld1     {v0.2d}, [x10]             /* {100, 200} */
    umlal   v0.2d, v14.2s, v7.s[2]     /* {110, 200 + 0xfffffffe*5} */
    umov    x0, v0.d[0]
    CKX     x0, 0x6e, 60
    umov    x0, v0.d[1]
    CKX     x0, 0x5000000be, 61
    mul     v0.4s, v14.4s, v7.s[2]     /* same-size: {10, -10, 20, 25} */
    umov    x0, v0.d[0]
    CKX     x0, 0xfffffff60000000a, 62
    umov    x0, v0.d[1]
    CKX     x0, 0x0000001900000014, 63

    /* three-different widening multiply (vector), XTN/XTN2, SHRN. */
    adr     x10, mbe_vn
    ld1     {v14.4s}, [x10]            /* {2, -2, 4, 5} */
    adr     x10, td_vm
    ld1     {v7.4s}, [x10]             /* {3, 7, 6, 9} */
    umull   v0.2d, v14.2s, v7.2s
    umov    x0, v0.d[0]
    CKX     x0, 0x6, 64
    umov    x0, v0.d[1]
    CKX     x0, 0x6fffffff2, 65
    smull   v0.2d, v14.2s, v7.2s       /* signed: d[1] = -2*7 = -14 */
    umov    x0, v0.d[1]
    CKX     x0, 0xfffffffffffffff2, 66
    umull2  v0.2d, v14.4s, v7.4s       /* high half {4*6, 5*9} */
    umov    x0, v0.d[1]
    CKX     x0, 0x2d, 67
    adr     x10, mbe_accd
    ld1     {v0.2d}, [x10]             /* {100, 200} */
    umlal   v0.2d, v14.2s, v7.2s
    umov    x0, v0.d[1]
    CKX     x0, 0x7000000ba, 68
    adr     x10, td_xt
    ld1     {v2.2d}, [x10]             /* {0x1122334455667788, 0x99aabbccddeeff00} */
    xtn     v1.2s, v2.2d               /* low 32 of each -> {0x55667788, 0xddeeff00} */
    umov    x0, v1.d[0]
    CKX     x0, 0xddeeff0055667788, 69
    shrn    v3.2s, v2.2d, #8           /* (each >> 8) truncated to 32 */
    umov    x0, v3.d[0]
    CKX     x0, 0xccddeeff44556677, 70
    movi    v4.4s, #0x11
    xtn2    v4.4s, v2.2d               /* high half := narrowed v2, keep low */
    umov    x0, v4.d[1]
    CKX     x0, 0xddeeff0055667788, 71

    /* SLI/SRI .4s #12 (ChaCha20 rotates use ushr+sli). */
    adr     x10, sli_vn
    ld1     {v1.4s}, [x10]
    adr     x10, sli_vd
    ld1     {v0.4s}, [x10]
    sli     v0.4s, v1.4s, #12
    umov    x0, v0.d[0]
    CKX     x0, 0xdef12abc00001fff, 72
    umov    x0, v0.d[1]
    CKX     x0, 0xfffff0ff00003678, 73
    adr     x10, sli_vd
    ld1     {v0.4s}, [x10]
    sri     v0.4s, v1.4s, #12
    umov    x0, v0.d[0]
    CKX     x0, 0x000abcde00000100, 74
    umov    x0, v0.d[1]
    CKX     x0, 0x000fffff12300000, 75

    /* ARMv8.2 SHA3 + SHA512 (FEAT_SHA3 / FEAT_SHA512). Reference values from
     * qemu-aarch64. .inst words avoid assembler .arch gating; all use fixed
     * regs v0=dest, v1=vA, v2=vB, v3=vC. */
    adr     x10, sha3_vA; ld1 {v1.2d}, [x10]
    adr     x10, sha3_vB; ld1 {v2.2d}, [x10]
    adr     x10, sha3_vC; ld1 {v3.2d}, [x10]

    .inst   0xce020c20                 /* eor3 v0.16b, v1.16b, v2.16b, v3.16b */
    umov    x0, v0.d[0]
    CKX     x0, 0xba98fedc32107654, 76
    umov    x0, v0.d[1]
    CKX     x0, 0x89abcdef01234567, 77

    .inst   0xce220c20                 /* bcax v0.16b, v1.16b, v2.16b, v3.16b */
    umov    x0, v0.d[0]
    CKX     x0, 0x1032547698badcfe, 78
    umov    x0, v0.d[1]
    CKX     x0, 0xdcfe98ba54761032, 79

    .inst   0xce628c20                 /* rax1 v0.2d, v1.2d, v2.2d */
    umov    x0, v0.d[0]
    CKX     x0, 0x23016745ab89efcd, 80
    umov    x0, v0.d[1]
    CKX     x0, 0xba98fedc32107654, 81

    .inst   0xce825020                 /* xar v0.2d, v1.2d, v2.2d, #20 */
    umov    x0, v0.d[0]
    CKX     x0, 0xadcfe1032547698b, 82
    umov    x0, v0.d[1]
    CKX     x0, 0x61032dcfe98ba547, 83

    mov     v0.16b, v3.16b             /* Vd = vC (source operand) */
    .inst   0xcec08020                 /* sha512su0 v0.2d, v1.2d */
    umov    x0, v0.d[0]
    CKX     x0, 0xa9ffffffffffffff, 84
    umov    x0, v0.d[1]
    CKX     x0, 0xc4e81cd1c1a46ff6, 85

    mov     v0.16b, v3.16b
    .inst   0xce628820                 /* sha512su1 v0.2d, v1.2d, v2.2d */
    umov    x0, v0.d[0]
    CKX     x0, 0x2c5f01c9778fed35, 86
    umov    x0, v0.d[1]
    CKX     x0, 0xead43169bba345fc, 87

    mov     v0.16b, v3.16b
    .inst   0xce628020                 /* sha512h q0, q1, v2.2d */
    umov    x0, v0.d[0]
    CKX     x0, 0x1378e216bd2e5b55, 88
    umov    x0, v0.d[1]
    CKX     x0, 0x4364ff20badc7698, 89

    mov     v0.16b, v3.16b
    .inst   0xce628420                 /* sha512h2 q0, q1, v2.2d */
    umov    x0, v0.d[0]
    CKX     x0, 0x9dea9dea9dea9de9, 90
    umov    x0, v0.d[1]
    CKX     x0, 0xcdefcdefcdefcdef, 91

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0

.balign 16
bytes0_15:
    .byte 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
su0_x:
    .word 0x11111111, 0x22222222, 0x33333333, 0x44444444
su0_y:
    .word 0x55555555, 0x66666666, 0x77777777, 0x88888888
pmull_a:
    .quad 0x123456789abcdef0
pmull_b:
    .quad 0x0fedcba987654321
tbl_table:
    .byte 0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
tbl_index:
    .byte 0x00,0x02,0x04,0x06,0x08,0x0a,0x0c,0x0e,0x10,0x01,0x03,0x11,0x05,0x07,0x09,0x0b
pw_n:
    .word 0xfffffffe, 0x00000005, 0x00000003, 0xfffffffd
pw_m:
    .word 0x00000009, 0xfffffffc, 0x00000008, 0x00000006
mbe_vn:
    .word 2, 0xfffffffe, 4, 5
mbe_vm:
    .word 9, 8, 5, 6
td_vm:
    .word 3, 7, 6, 9
.balign 16
mbe_accd:
    .quad 100, 200
td_xt:
    .quad 0x1122334455667788, 0x99aabbccddeeff00
sli_vn:
    .word 0x00100001, 0xabcdef12, 0x00000003, 0xffffffff
sli_vd:
    .word 0x00000fff, 0x00000abc, 0x12345678, 0x000000ff
.balign 16
sha3_vA:
    .quad 0x0123456789abcdef, 0xfedcba9876543210
sha3_vB:
    .quad 0x1111111111111111, 0x2222222222222222
sha3_vC:
    .quad 0xaaaaaaaaaaaaaaaa, 0x5555555555555555

.balign 16
sha_iv:
    .word 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a
    .word 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
sha_msg:
    .byte 0x61, 0x62, 0x63, 0x80
    .zero 52
    .byte 0, 0, 0, 0, 0, 0, 0, 0x18
sha_k:
    .word 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5
    .word 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5
    .word 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3
    .word 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174
    .word 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc
    .word 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da
    .word 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7
    .word 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967
    .word 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13
    .word 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85
    .word 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3
    .word 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070
    .word 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5
    .word 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3
    .word 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208
    .word 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
