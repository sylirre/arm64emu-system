/* M4 FP/SIMD self-test: MOVI/MVNI/DUP/INS/UMOV/SMOV + vector load/store.
 * x0=0 on success, else failing-check id. */
.macro CKX reg, val, id
    movz    x11, #\id
    ldr     x9, =\val
    cmp     \reg, x9
    b.ne    fail
.endm

.text
.global _start
_start:
    movz    x20, #0x4020, lsl #16       /* scratch 0x40200000 */

    /* MOVI Vd.16B, #0x12  -> all bytes 0x12 */
    movi    v0.16b, #0x12
    umov    x0, v0.d[0]
    CKX     x0, 0x1212121212121212, 1
    umov    x0, v0.d[1]
    CKX     x0, 0x1212121212121212, 2

    /* MOVI Vd.4S, #0xab, LSL #8 -> each 32-bit lane 0x0000ab00 */
    movi    v1.4s, #0xab, lsl #8
    umov    w0, v1.s[0]
    CKX     x0, 0x0000ab00, 3
    umov    w0, v1.s[3]
    CKX     x0, 0x0000ab00, 4

    /* MVNI Vd.4S, #0x00, LSL #0 -> ~0 = 0xffffffff per lane */
    mvni    v2.4s, #0x00
    umov    w0, v2.s[1]
    CKX     x0, 0xffffffff, 5

    /* DUP general: V3.4S = w-value */
    movz    w5, #0x3456
    movk    w5, #0x12, lsl #16          /* 0x00123456 */
    dup     v3.4s, w5
    umov    w0, v3.s[2]
    CKX     x0, 0x00123456, 6

    /* DUP element: V4.8H = V3.H[0] (0x3456) */
    dup     v4.8h, v3.h[0]
    umov    w0, v4.h[5]
    CKX     x0, 0x3456, 7

    /* INS general then element */
    movz    x6, #0xbeef
    ins     v5.d[1], x6                 /* V5.D[1] = 0xbeef */
    umov    x0, v5.d[1]
    CKX     x0, 0xbeef, 8
    ins     v5.d[0], v5.d[1]            /* copy hi->lo */
    umov    x0, v5.d[0]
    CKX     x0, 0xbeef, 9

    /* SMOV: sign-extend a byte */
    movi    v6.16b, #0x80
    smov    x0, v6.b[0]                 /* 0x80 sign-extended */
    CKX     x0, 0xffffffffffffff80, 10

    /* vector load/store: STR Q, LDR Q round-trip */
    movz    x7, #0xcafe
    movk    x7, #0xdead, lsl #16
    dup     v7.2d, x7                   /* both 64-bit lanes = 0x...deadcafe */
    str     q7, [x20]
    ldr     q8, [x20]
    umov    x0, v8.d[0]
    CKX     x0, 0xdeadcafe, 11
    umov    x0, v8.d[1]
    CKX     x0, 0xdeadcafe, 12

    /* STP/LDP of D registers */
    movi    v9.8b, #0x01
    movi    v10.8b, #0x02
    stp     d9, d10, [x20, #32]
    ldp     d11, d12, [x20, #32]
    umov    x0, v11.d[0]
    CKX     x0, 0x0101010101010101, 13
    umov    x0, v12.d[0]
    CKX     x0, 0x0202020202020202, 14

    /* Compare-against-zero (two-reg misc). v0.8b bytes (lane0..7):
     * 0x00(0) 0x01(+1) 0x7f(+127) 0x80(-128) 0xff(-1) 0x00(0) 0x40(+64) 0xc0(-64)
     * Guards the CMEQ#0 / CMGE#0 distinction (null-byte test in libc strlen). */
    movz    x5, #0x0100
    movk    x5, #0x807f, lsl #16
    movk    x5, #0x00ff, lsl #32
    movk    x5, #0xc040, lsl #48
    fmov    d0, x5
    cmeq    v1.8b, v0.8b, #0            /* ==0   */
    umov    x0, v1.d[0]
    CKX     x0, 0x0000ff00000000ff, 15
    cmgt    v2.8b, v0.8b, #0            /* >0  (signed) */
    umov    x0, v2.d[0]
    CKX     x0, 0x00ff000000ffff00, 16
    cmge    v3.8b, v0.8b, #0            /* >=0 (signed) */
    umov    x0, v3.d[0]
    CKX     x0, 0x00ffff0000ffffff, 17
    cmle    v4.8b, v0.8b, #0            /* <=0 (signed) */
    umov    x0, v4.d[0]
    CKX     x0, 0xff00ffffff0000ff, 18
    cmlt    v5.8b, v0.8b, #0            /* <0  (signed) */
    umov    x0, v5.d[0]
    CKX     x0, 0xff0000ffff000000, 19

    movz    x0, #0
    hlt     #0
fail:
    mov     x0, x11
    hlt     #0
