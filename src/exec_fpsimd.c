/* AArch64 FP/Advanced-SIMD execution. Grown on demand as the firmware/kernel
 * exercise instructions (the "implement on demand" M4 strategy). */
#include "cpu.h"
#include "esr.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define BIT(i)      ((insn >> (i)) & 1u)
#define BITS(hi,lo) ((insn >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

static void fpsimd_undef(CPU *c, u32 insn) {
    /* Always log: drives the on-demand FP/SIMD implementation loop. */
    fprintf(stderr, "[fpsimd] UNIMPL 0x%08x at pc=0x%llx\n",
            insn, (unsigned long long)c->cur_insn_pc);
    cpu_raise_sync(c, esr_make(EC_UNKNOWN, 0), 0);
}

static u64 rep8(u64 b)  { b &= 0xff;   return b * 0x0101010101010101ULL; }
static u64 rep16(u64 h) { h &= 0xffff; return h * 0x0001000100010001ULL; }
static u64 rep32(u64 w) { w &= 0xffffffff; return w | (w << 32); }

/* VFP modified-immediate expansion (for FMOV vector/scalar). */
static u32 vfp_imm32(unsigned imm8) {
    /* sign:imm8<7>  exp8 = NOT(b6):Replicate(b6,5):imm8<5:4>  frac = imm8<3:0> */
    unsigned s = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1, e = (imm8 >> 4) & 3;
    u32 exp8 = ((!b6) << 7) | ((b6 ? 0x1f : 0) << 2) | e;
    return ((u32)s << 31) | (exp8 << 23) | ((u32)(imm8 & 0xf) << 19);
}
static u64 vfp_imm64(unsigned imm8) {
    unsigned s = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
    unsigned e = ((imm8 >> 4) & 3);
    u64 sign = (u64)s << 63;
    u64 exp11 = ((u64)(!b6) << 10) | ((u64)(b6 ? 0xff : 0) << 2) | e;  /* exp[10]=NOT b6, [9:2]=Rep(b6,8), [1:0]=imm8[5:4] */
    u64 frac = (u64)(imm8 & 0xf) << 48;
    return sign | (exp11 << 52) | frac;
}

/* AdvSIMD modified immediate -> 64-bit element pattern (MOVI value). */
static u64 expand_imm(unsigned op, unsigned cmode, unsigned imm8) {
    unsigned hi = (cmode >> 1) & 7, lo = cmode & 1;
    switch (hi) {
        case 0: return rep32(imm8);
        case 1: return rep32((u64)imm8 << 8);
        case 2: return rep32((u64)imm8 << 16);
        case 3: return rep32((u64)imm8 << 24);
        case 4: return rep16(imm8);
        case 5: return rep16((u64)imm8 << 8);
        case 6: return lo ? rep32(((u64)imm8 << 16) | 0xffff)
                          : rep32(((u64)imm8 << 8) | 0xff);
        default: /* 7 */
            if (lo == 0 && op == 0) return rep8(imm8);
            if (lo == 0 && op == 1) {           /* MOVI 64-bit: bit->byte expand */
                u64 v = 0;
                for (int i = 0; i < 8; i++) if ((imm8 >> i) & 1) v |= 0xffULL << (i * 8);
                return v;
            }
            if (lo == 1 && op == 0) return rep32(vfp_imm32(imm8));   /* FMOV .4S */
            return vfp_imm64(imm8);                                  /* FMOV .2D */
    }
}

static void simd_modified_imm(CPU *c, u32 insn) {
    unsigned Q = BIT(30), op = BIT(29), cmode = BITS(15, 12), Rd = BITS(4, 0);
    unsigned imm8 = (BITS(18, 16) << 5) | BITS(9, 5);
    unsigned hi = (cmode >> 1) & 7, lo = cmode & 1;

    bool orr_bic = (lo == 1) && (hi <= 5);   /* ORR/BIC (vector, immediate) */
    u64 v;
    if (orr_bic) {
        v = expand_imm(0, cmode, imm8);
        if (op == 0) { c->v[Rd].d[0] |= v; if (Q) c->v[Rd].d[1] |= v; }   /* ORR */
        else { c->v[Rd].d[0] &= ~v; if (Q) c->v[Rd].d[1] &= ~v; }         /* BIC */
        if (!Q) c->v[Rd].d[1] = 0;
        return;
    }
    v = expand_imm(op, cmode, imm8);
    /* MVNI: op==1 inverts, except the two cmode==111x special MOVI/FMOV forms */
    if (op == 1 && !(hi == 7)) v = ~v;
    c->v[Rd].d[0] = v;
    c->v[Rd].d[1] = Q ? v : 0;
}

static u64 velem_get(const V128 *v, unsigned size, unsigned idx) {
    switch (size) {
        case 0: return v->b[idx];
        case 1: return v->h[idx];
        case 2: return v->s[idx];
        default: return v->d[idx];
    }
}
static void velem_set(V128 *v, unsigned size, unsigned idx, u64 val) {
    switch (size) {
        case 0: v->b[idx] = (u8)val; break;
        case 1: v->h[idx] = (u16)val; break;
        case 2: v->s[idx] = (u32)val; break;
        default: v->d[idx] = val; break;
    }
}

/* AdvSIMD copy: DUP/INS/UMOV/SMOV. */
static void simd_copy(CPU *c, u32 insn) {
    unsigned Q = BIT(30), op = BIT(29);
    unsigned imm5 = BITS(20, 16), imm4 = BITS(14, 11);
    unsigned Rn = BITS(9, 5), Rd = BITS(4, 0);
    /* element size = position of lowest set bit in imm5 */
    unsigned size = (imm5 & 1) ? 0 : (imm5 & 2) ? 1 : (imm5 & 4) ? 2 : 3;
    unsigned index = imm5 >> (size + 1);

    if (op == 1) {                                  /* INS (element): Vd[idx1]=Vn[idx2] */
        unsigned idx2 = imm4 >> size;
        velem_set(&c->v[Rd], size, index, velem_get(&c->v[Rn], size, idx2));
        return;
    }
    switch (imm4) {
        case 0x0: {                                 /* DUP (element) */
            u64 e = velem_get(&c->v[Rn], size, index);
            V128 r; r.d[0] = r.d[1] = 0;
            unsigned lanes = (16 >> size) >> (Q ? 0 : 1);
            for (unsigned i = 0; i < lanes; i++) velem_set(&r, size, i, e);
            c->v[Rd] = r;
            break;
        }
        case 0x1: {                                 /* DUP (general): lanes = Xn */
            u64 e = reg_x(c, Rn);
            V128 r; r.d[0] = r.d[1] = 0;
            unsigned lanes = (16 >> size) >> (Q ? 0 : 1);
            for (unsigned i = 0; i < lanes; i++) velem_set(&r, size, i, e);
            c->v[Rd] = r;
            break;
        }
        case 0x3:                                   /* INS (general): Vd[idx]=Xn */
            velem_set(&c->v[Rd], size, index, reg_x(c, Rn));
            break;
        case 0x5: {                                 /* SMOV */
            u64 e = velem_get(&c->v[Rn], size, index);
            u64 v = sign_extend(e, 8u << size);
            set_x(c, Rd, Q ? v : (u32)v);
            break;
        }
        case 0x7: {                                 /* UMOV */
            u64 e = velem_get(&c->v[Rn], size, index);
            set_x(c, Rd, Q ? e : (u32)e);
            break;
        }
        default: fpsimd_undef(c, insn); break;
    }
}

/* ---------------- Scalar floating-point (single/double) ---------------- *
 * Implemented with the host's IEEE-754 float/double (the host is IEEE-754 LE,
 * matching the guest). Covers convert, arithmetic, compare, select, FMOV and
 * fused multiply-add — the scalar FP the firmware and Linux rely on. Half
 * precision (ftype=11) and fixed-point convert scales beyond the common cases
 * fall through to UNDEF and surface via the on-demand trap. */
static double fp_rd_d(CPU *c, unsigned n) { double x; u64 b = c->v[n].d[0]; memcpy(&x, &b, 8); return x; }
static float  fp_rd_s(CPU *c, unsigned n) { float  x; u32 b = c->v[n].s[0]; memcpy(&x, &b, 4); return x; }
static void fp_wr_d(CPU *c, unsigned d, double x) { u64 b; memcpy(&b, &x, 8); c->v[d].d[0] = b; c->v[d].d[1] = 0; }
static void fp_wr_s(CPU *c, unsigned d, float  x) { u32 b; memcpy(&b, &x, 4); c->v[d].d[0] = b; c->v[d].d[1] = 0; }

/* Rounding without libm: values >= 2^52 are already integral; otherwise round
 * via an integer cast (which truncates toward zero) and adjust. __builtin_fabs
 * and __builtin_sqrt are inlined to hardware ops (see -fno-math-errno). */
#define FP_INTEGRAL 4503599627370496.0   /* 2^52 */
static double f_trunc(double v) {
    if (!(v == v) || __builtin_fabs(v) >= FP_INTEGRAL) return v;
    return (double)(s64)v;
}
static double f_floor(double v) {
    if (!(v == v) || __builtin_fabs(v) >= FP_INTEGRAL) return v;
    double t = (double)(s64)v; return (t > v) ? t - 1.0 : t;
}
static double f_ceil(double v) {
    if (!(v == v) || __builtin_fabs(v) >= FP_INTEGRAL) return v;
    double t = (double)(s64)v; return (t < v) ? t + 1.0 : t;
}
static double f_round(double v) {            /* nearest, ties away from zero */
    return (v >= 0) ? f_floor(v + 0.5) : f_ceil(v - 0.5);
}

/* Set NZCV from an FP compare of a vs b (ordered), per the architecture. */
static void fp_set_flags(CPU *c, int cmp /* -1 lt, 0 eq, 1 gt, 2 unordered */) {
    u32 n = 0;
    switch (cmp) {
        case -1: n = PS_N; break;                 /* less than       N=1 */
        case  0: n = PS_Z | PS_C; break;          /* equal           Z,C */
        case  1: n = PS_C; break;                 /* greater than    C   */
        default: n = PS_C | PS_V; break;          /* unordered       C,V */
    }
    c->nzcv = n;
}

static void exec_fp_scalar(CPU *c, u32 insn) {
    unsigned ftype = BITS(23, 22), Rn = BITS(9, 5), Rd = BITS(4, 0);
    bool dbl = (ftype == 1);

    /* FMOV to/from the high 64 bits of a SIMD reg (ptype=10 => 128-bit variant,
     * NOT half-precision). Lives in the FP<->integer conversion space; must be
     * caught before the ftype!=0/1 half-precision deferral below. */
    if (BITS(28, 24) == 0x1e && BIT(21) == 1 && BITS(15, 10) == 0 &&
        BIT(31) == 1 && ftype == 2 && BITS(20, 19) == 1) {
        unsigned opcode = BITS(18, 16);
        if (opcode == 6) { set_x(c, Rd, c->v[Rn].d[1]); return; }   /* FMOV Xd, Vn.D[1] */
        if (opcode == 7) { c->v[Rd].d[1] = reg_x(c, Rn); return; }  /* FMOV Vd.D[1], Xn */
    }

    if (ftype != 0 && ftype != 1) { fpsimd_undef(c, insn); return; }  /* half: on demand */

    /* Floating-point <-> integer / fixed-point conversion (bit21 distinguishes). */
    if (BITS(28, 24) == 0x1e && BIT(21) == 1 && BITS(15, 10) == 0) {
        unsigned sf = BIT(31), rmode = BITS(20, 19), opcode = BITS(18, 16);
        bool x64 = sf != 0;
        switch ((rmode << 3) | opcode) {
            case (0 << 3) | 2:  /* SCVTF (signed int -> fp) */
                if (dbl) fp_wr_d(c, Rd, x64 ? (double)(s64)reg_x(c, Rn) : (double)(s32)reg_x(c, Rn));
                else     fp_wr_s(c, Rd, x64 ? (float)(s64)reg_x(c, Rn)  : (float)(s32)reg_x(c, Rn));
                return;
            case (0 << 3) | 3:  /* UCVTF (unsigned int -> fp) */
                if (dbl) fp_wr_d(c, Rd, x64 ? (double)(u64)reg_x(c, Rn) : (double)(u32)reg_x(c, Rn));
                else     fp_wr_s(c, Rd, x64 ? (float)(u64)reg_x(c, Rn)  : (float)(u32)reg_x(c, Rn));
                return;
            case (0 << 3) | 6:  /* FMOV (fp -> general) */
                if (dbl) set_x(c, Rd, c->v[Rn].d[0]); else set_x(c, Rd, c->v[Rn].s[0]);
                return;
            case (0 << 3) | 7:  /* FMOV (general -> fp) */
                if (dbl) { c->v[Rd].d[0] = reg_x(c, Rn); c->v[Rd].d[1] = 0; }
                else     { c->v[Rd].d[0] = (u32)reg_x(c, Rn); c->v[Rd].d[1] = 0; }
                return;
            default: break;
        }
        /* FCVT*-to-integer: rmode selects rounding; opcode 0=..S 1=..U.
         * We implement round-toward-zero (Z), nearest (N) and away (A); P/M map
         * to ceil/floor. Saturation matches the architecture's clamping. */
        if (opcode <= 1) {
            double v = dbl ? fp_rd_d(c, Rn) : (double)fp_rd_s(c, Rn);
            double r;
            switch (rmode) {
                case 0: r = f_round(v); break;   /* N: nearest (ties away) */
                case 1: r = f_ceil(v);  break;   /* P: +inf */
                case 2: r = f_floor(v); break;   /* M: -inf */
                default: r = f_trunc(v); break;  /* Z: toward zero */
            }
            if (opcode == 0) {   /* signed */
                if (x64) { s64 m = (r >= 9223372036854775807.0) ? INT64_MAX : (r <= -9223372036854775808.0) ? INT64_MIN : (s64)r; set_x(c, Rd, (u64)m); }
                else     { s32 m = (r >= 2147483647.0) ? INT32_MAX : (r <= -2147483648.0) ? INT32_MIN : (s32)r; set_x(c, Rd, (u64)(u32)m); }
            } else {             /* unsigned */
                if (r < 0) r = 0;
                if (x64) { u64 m = (r >= 18446744073709551615.0) ? UINT64_MAX : (u64)r; set_x(c, Rd, m); }
                else     { u32 m = (r >= 4294967295.0) ? UINT32_MAX : (u32)r; set_x(c, Rd, m); }
            }
            return;
        }
        fpsimd_undef(c, insn); return;
    }

    /* Floating-point <-> fixed-point conversion (bit21==0): the integer side
     * carries #fbits fractional bits (fbits = 64 - scale). SCVTF/UCVTF divide
     * the integer by 2^fbits; FCVTZS/FCVTZU multiply the float by 2^fbits then
     * round toward zero (saturating). Emitted by libc float formatting. */
    if (BIT(21) == 0) {
        unsigned sf = BIT(31), rmode = BITS(20, 19), opcode = BITS(18, 16);
        unsigned fbits = 64 - BITS(15, 10);
        bool x64 = sf != 0;
        u64 pb = (u64)(fbits + 1023) << 52; double pow2; memcpy(&pow2, &pb, 8); /* 2^fbits, exact */
        if (rmode == 0 && (opcode == 2 || opcode == 3)) {       /* SCVTF / UCVTF: fixed -> fp */
            if (dbl) {
                double iv = (opcode == 2) ? (x64 ? (double)(s64)reg_x(c, Rn) : (double)(s32)reg_x(c, Rn))
                                          : (x64 ? (double)(u64)reg_x(c, Rn) : (double)(u32)reg_x(c, Rn));
                fp_wr_d(c, Rd, iv / pow2);
            } else {
                float iv = (opcode == 2) ? (x64 ? (float)(s64)reg_x(c, Rn) : (float)(s32)reg_x(c, Rn))
                                         : (x64 ? (float)(u64)reg_x(c, Rn) : (float)(u32)reg_x(c, Rn));
                fp_wr_s(c, Rd, iv / (float)pow2);
            }
            return;
        }
        if (rmode == 3 && (opcode == 0 || opcode == 1)) {       /* FCVTZS / FCVTZU: fp -> fixed */
            double r = f_trunc((dbl ? fp_rd_d(c, Rn) : (double)fp_rd_s(c, Rn)) * pow2);
            if (opcode == 0) {   /* signed */
                if (x64) { s64 m = (r >= 9223372036854775807.0) ? INT64_MAX : (r <= -9223372036854775808.0) ? INT64_MIN : (s64)r; set_x(c, Rd, (u64)m); }
                else     { s32 m = (r >= 2147483647.0) ? INT32_MAX : (r <= -2147483648.0) ? INT32_MIN : (s32)r; set_x(c, Rd, (u64)(u32)m); }
            } else {             /* unsigned */
                if (r < 0) r = 0;
                if (x64) { u64 m = (r >= 18446744073709551615.0) ? UINT64_MAX : (u64)r; set_x(c, Rd, m); }
                else     { u32 m = (r >= 4294967295.0) ? UINT32_MAX : (u32)r; set_x(c, Rd, m); }
            }
            return;
        }
        fpsimd_undef(c, insn); return;   /* other bit21==0 encodings: on demand */
    }

    unsigned o2 = BITS(11, 10);
    if (o2 == 0) {
        if (BIT(12) == 1) {                       /* FP immediate */
            unsigned imm8 = BITS(20, 13);
            if (dbl) { u64 b = vfp_imm64(imm8); double t; memcpy(&t, &b, 8); fp_wr_d(c, Rd, t); }
            else     { u32 b = vfp_imm32(imm8); float  t; memcpy(&t, &b, 4); fp_wr_s(c, Rd, t); }
            return;
        }
        if (BIT(13) == 1) {                       /* FP compare */
            unsigned Rm = BITS(20, 16);
            bool with_zero = BIT(3);              /* opcode2<3>: compare with 0.0 */
            int cmp;
            if (dbl) {
                double a = fp_rd_d(c, Rn), b = with_zero ? 0.0 : fp_rd_d(c, Rm);
                cmp = (a < b) ? -1 : (a == b) ? 0 : (a > b) ? 1 : 2;
            } else {
                float a = fp_rd_s(c, Rn), b = with_zero ? 0.0f : fp_rd_s(c, Rm);
                cmp = (a < b) ? -1 : (a == b) ? 0 : (a > b) ? 1 : 2;
            }
            fp_set_flags(c, cmp);
            return;
        }
        if (BIT(14) == 1) {                       /* FP data-processing (1 source) */
            unsigned opc = BITS(20, 15);
            switch (opc) {
                case 0x0: if (dbl) fp_wr_d(c, Rd, fp_rd_d(c, Rn)); else fp_wr_s(c, Rd, fp_rd_s(c, Rn)); return; /* FMOV */
                case 0x1: if (dbl) fp_wr_d(c, Rd, __builtin_fabs(fp_rd_d(c, Rn))); else fp_wr_s(c, Rd, __builtin_fabsf(fp_rd_s(c, Rn))); return; /* FABS */
                case 0x2: if (dbl) fp_wr_d(c, Rd, -fp_rd_d(c, Rn)); else fp_wr_s(c, Rd, -fp_rd_s(c, Rn)); return; /* FNEG */
                case 0x3: if (dbl) fp_wr_d(c, Rd, __builtin_sqrt(fp_rd_d(c, Rn))); else fp_wr_s(c, Rd, __builtin_sqrtf(fp_rd_s(c, Rn))); return; /* FSQRT */
                case 0x4: if (ftype == 1) fp_wr_s(c, Rd, (float)fp_rd_d(c, Rn)); else fpsimd_undef(c, insn); return;  /* FCVT to single (from double) */
                case 0x5: if (ftype == 0) fp_wr_d(c, Rd, (double)fp_rd_s(c, Rn)); else fpsimd_undef(c, insn); return; /* FCVT to double (from single) */
                case 0x8: if (dbl) fp_wr_d(c, Rd, f_round(fp_rd_d(c, Rn))); else fp_wr_s(c, Rd, (float)f_round(fp_rd_s(c, Rn))); return; /* FRINTN ~ */
                case 0x9: if (dbl) fp_wr_d(c, Rd, f_ceil(fp_rd_d(c, Rn))); else fp_wr_s(c, Rd, (float)f_ceil(fp_rd_s(c, Rn))); return;   /* FRINTP */
                case 0xa: if (dbl) fp_wr_d(c, Rd, f_floor(fp_rd_d(c, Rn))); else fp_wr_s(c, Rd, (float)f_floor(fp_rd_s(c, Rn))); return;  /* FRINTM */
                case 0xb: if (dbl) fp_wr_d(c, Rd, f_trunc(fp_rd_d(c, Rn))); else fp_wr_s(c, Rd, (float)f_trunc(fp_rd_s(c, Rn))); return;  /* FRINTZ */
                default: fpsimd_undef(c, insn); return;
            }
        }
        fpsimd_undef(c, insn); return;
    }

    if (o2 == 2) {                                /* FP data-processing (2 source) */
        unsigned Rm = BITS(20, 16), opc = BITS(15, 12);
        if (dbl) {
            double a = fp_rd_d(c, Rn), b = fp_rd_d(c, Rm), r;
            switch (opc) {
                case 0x0: r = a * b; break;                    /* FMUL */
                case 0x1: r = a / b; break;                    /* FDIV */
                case 0x2: r = a + b; break;                    /* FADD */
                case 0x3: r = a - b; break;                    /* FSUB */
                case 0x4: r = (a > b) ? a : b; break;          /* FMAX */
                case 0x5: r = (a < b) ? a : b; break;          /* FMIN */
                case 0x6: r = (a > b) ? a : b; break;          /* FMAXNM ~ */
                case 0x7: r = (a < b) ? a : b; break;          /* FMINNM ~ */
                case 0x8: r = -(a * b); break;                 /* FNMUL */
                default: fpsimd_undef(c, insn); return;
            }
            fp_wr_d(c, Rd, r);
        } else {
            float a = fp_rd_s(c, Rn), b = fp_rd_s(c, Rm), r;
            switch (opc) {
                case 0x0: r = a * b; break;
                case 0x1: r = a / b; break;
                case 0x2: r = a + b; break;
                case 0x3: r = a - b; break;
                case 0x4: r = (a > b) ? a : b; break;
                case 0x5: r = (a < b) ? a : b; break;
                case 0x6: r = (a > b) ? a : b; break;
                case 0x7: r = (a < b) ? a : b; break;
                case 0x8: r = -(a * b); break;
                default: fpsimd_undef(c, insn); return;
            }
            fp_wr_s(c, Rd, r);
        }
        return;
    }

    if (o2 == 3) {                                /* FP conditional select (FCSEL) */
        unsigned Rm = BITS(20, 16), cond = BITS(15, 12);
        unsigned src = cond_holds(c, cond) ? Rn : Rm;
        if (dbl) fp_wr_d(c, Rd, fp_rd_d(c, src)); else fp_wr_s(c, Rd, fp_rd_s(c, src));
        return;
    }

    if (o2 == 1) {                                /* FP conditional compare (FCCMP) */
        unsigned Rm = BITS(20, 16), cond = BITS(15, 12), nzcv = BITS(3, 0);
        if (cond_holds(c, cond)) {
            int cmp;
            if (dbl) { double a = fp_rd_d(c, Rn), b = fp_rd_d(c, Rm); cmp = (a < b) ? -1 : (a == b) ? 0 : (a > b) ? 1 : 2; }
            else     { float  a = fp_rd_s(c, Rn), b = fp_rd_s(c, Rm); cmp = (a < b) ? -1 : (a == b) ? 0 : (a > b) ? 1 : 2; }
            fp_set_flags(c, cmp);
        } else {
            c->nzcv = ((nzcv & 8) ? PS_N : 0) | ((nzcv & 4) ? PS_Z : 0) |
                      ((nzcv & 2) ? PS_C : 0) | ((nzcv & 1) ? PS_V : 0);
        }
        return;
    }

    fpsimd_undef(c, insn);
}

/* Scalar FP fused multiply-add family (3 source): FMADD/FMSUB/FNMADD/FNMSUB. */
static void exec_fp_dp3(CPU *c, u32 insn) {
    unsigned ftype = BITS(23, 22), Rm = BITS(20, 16), Ra = BITS(14, 10);
    unsigned Rn = BITS(9, 5), Rd = BITS(4, 0), o1 = BIT(21), o0 = BIT(15);
    if (ftype != 0 && ftype != 1) { fpsimd_undef(c, insn); return; }
    if (ftype == 1) {
        double n = fp_rd_d(c, Rn), m = fp_rd_d(c, Rm), a = fp_rd_d(c, Ra), r;
        if (!o1 && !o0) r =  a + n * m;           /* FMADD  */
        else if (!o1)   r =  a - n * m;           /* FMSUB  */
        else if (o1 && !o0) r = -a - n * m;       /* FNMADD */
        else            r = -a + n * m;           /* FNMSUB */
        fp_wr_d(c, Rd, r);
    } else {
        float n = fp_rd_s(c, Rn), m = fp_rd_s(c, Rm), a = fp_rd_s(c, Ra), r;
        if (!o1 && !o0) r =  a + n * m;
        else if (!o1)   r =  a - n * m;
        else if (o1 && !o0) r = -a - n * m;
        else            r = -a + n * m;
        fp_wr_s(c, Rd, r);
    }
}

/* Sign-extend an `esize`-bit element value to s64. */
static s64 sx(u64 v, unsigned esize) { return (s64)sign_extend(v, esize); }

/* AdvSIMD three-same (vector integer): element-wise ADD/SUB/compare/min/max/mul
 * and the bitwise logical group. The workhorse vector group for string/memory
 * routines (CMEQ/CMHS...) in EDK2 and Linux. */
static void simd_three_same(CPU *c, u32 insn) {
    unsigned Q = BIT(30), U = BIT(29), size = BITS(23, 22);
    unsigned Rm = BITS(20, 16), opc = BITS(15, 11), Rn = BITS(9, 5), Rd = BITS(4, 0);

    if (opc == 0x03) {                         /* logical (size selects op) */
        u64 a0 = c->v[Rn].d[0], a1 = c->v[Rn].d[1];
        u64 b0 = c->v[Rm].d[0], b1 = c->v[Rm].d[1];
        u64 d0 = c->v[Rd].d[0], d1 = c->v[Rd].d[1], r0, r1;
        if (!U) switch (size) {
            case 0: r0 = a0 & b0;  r1 = a1 & b1;  break;          /* AND */
            case 1: r0 = a0 & ~b0; r1 = a1 & ~b1; break;          /* BIC */
            case 2: r0 = a0 | b0;  r1 = a1 | b1;  break;          /* ORR */
            default: r0 = a0 | ~b0; r1 = a1 | ~b1; break;         /* ORN */
        } else switch (size) {
            case 0: r0 = a0 ^ b0; r1 = a1 ^ b1; break;            /* EOR */
            case 1: r0 = b0 ^ ((b0 ^ a0) & d0); r1 = b1 ^ ((b1 ^ a1) & d1); break; /* BSL */
            case 2: r0 = d0 ^ ((d0 ^ a0) & b0); r1 = d1 ^ ((d1 ^ a1) & b1); break; /* BIT */
            default: r0 = d0 ^ ((d0 ^ a0) & ~b0); r1 = d1 ^ ((d1 ^ a1) & ~b1); break; /* BIF */
        }
        c->v[Rd].d[0] = r0; c->v[Rd].d[1] = Q ? r1 : 0;
        return;
    }

    unsigned esize = 8u << size, n = (Q ? 16 : 8) >> size;
    V128 r; r.d[0] = r.d[1] = 0;
    u64 emask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);

    /* Pairwise: ADDP (0x17, U=0), S/UMAXP (0x14), S/UMINP (0x15). Output lower
     * half folds pairs of Rn, upper half folds pairs of Rm. */
    if ((opc == 0x17 && U == 0) || opc == 0x14 || opc == 0x15) {
        for (unsigned i = 0; i < n / 2; i++) {
            u64 n0 = velem_get(&c->v[Rn], size, 2*i), n1 = velem_get(&c->v[Rn], size, 2*i + 1);
            u64 m0 = velem_get(&c->v[Rm], size, 2*i), m1 = velem_get(&c->v[Rm], size, 2*i + 1);
            u64 lo, hi;
            if (opc == 0x17)        { lo = n0 + n1; hi = m0 + m1; }                 /* ADDP */
            else if (opc == 0x14) {                                                /* MAXP */
                lo = (U ? n0 > n1 : sx(n0,esize) > sx(n1,esize)) ? n0 : n1;
                hi = (U ? m0 > m1 : sx(m0,esize) > sx(m1,esize)) ? m0 : m1;
            } else {                                                               /* MINP */
                lo = (U ? n0 < n1 : sx(n0,esize) < sx(n1,esize)) ? n0 : n1;
                hi = (U ? m0 < m1 : sx(m0,esize) < sx(m1,esize)) ? m0 : m1;
            }
            velem_set(&r, size, i, lo & emask);
            velem_set(&r, size, n / 2 + i, hi & emask);
        }
        c->v[Rd] = r;
        return;
    }

    for (unsigned i = 0; i < n; i++) {
        u64 a = velem_get(&c->v[Rn], size, i), b = velem_get(&c->v[Rm], size, i), v;
        switch ((U << 5) | opc) {
            case (0 << 5) | 0x10: v = a + b; break;                       /* ADD */
            case (1 << 5) | 0x10: v = a - b; break;                       /* SUB */
            case (0 << 5) | 0x11: v = (a & b) ? emask : 0; break;         /* CMTST */
            case (1 << 5) | 0x11: v = (a == b) ? emask : 0; break;        /* CMEQ */
            case (0 << 5) | 0x06: v = (sx(a,esize) >  sx(b,esize)) ? emask : 0; break; /* CMGT */
            case (1 << 5) | 0x06: v = (a >  b) ? emask : 0; break;        /* CMHI */
            case (0 << 5) | 0x07: v = (sx(a,esize) >= sx(b,esize)) ? emask : 0; break; /* CMGE */
            case (1 << 5) | 0x07: v = (a >= b) ? emask : 0; break;        /* CMHS */
            case (0 << 5) | 0x0c: v = (sx(a,esize) >  sx(b,esize)) ? a : b; break;     /* SMAX */
            case (1 << 5) | 0x0c: v = (a > b) ? a : b; break;            /* UMAX */
            case (0 << 5) | 0x0d: v = (sx(a,esize) <  sx(b,esize)) ? a : b; break;     /* SMIN */
            case (1 << 5) | 0x0d: v = (a < b) ? a : b; break;            /* UMIN */
            case (0 << 5) | 0x13: v = a * b; break;                       /* MUL */
            case (0 << 5) | 0x12: v = velem_get(&c->v[Rd], size, i) + a * b; break;    /* MLA */
            case (1 << 5) | 0x12: v = velem_get(&c->v[Rd], size, i) - a * b; break;    /* MLS */
            case (0 << 5) | 0x01: v = a + b; break;                       /* (SQADD approx) */
            default: fpsimd_undef(c, insn); return;
        }
        velem_set(&r, size, i, v & emask);
    }
    c->v[Rd] = r;
}

/* AdvSIMD across-lanes reductions: ADDV/UMAXV/UMINV/SMAXV/SMINV (horizontal
 * reduce of a vector to a scalar element in Rd). Used by string routines to
 * collapse a per-byte compare mask into a single found/not-found value. */
static void simd_across(CPU *c, u32 insn) {
    unsigned Q = BIT(30), U = BIT(29), size = BITS(23, 22);
    unsigned opc = BITS(16, 12), Rn = BITS(9, 5), Rd = BITS(4, 0);
    unsigned esize = 8u << size, n = (Q ? 16 : 8) >> size;
    u64 emask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
    u64 acc = velem_get(&c->v[Rn], size, 0);
    for (unsigned i = 1; i < n; i++) {
        u64 e = velem_get(&c->v[Rn], size, i);
        switch ((U << 5) | opc) {
            case (0 << 5) | 0x1b: acc += e; break;                                  /* ADDV */
            case (0 << 5) | 0x03: case (1 << 5) | 0x03: acc += e; break;            /* SADDLV/UADDLV */
            case (0 << 5) | 0x0a: acc = (sx(acc,esize) > sx(e,esize)) ? acc : e; break; /* SMAXV */
            case (1 << 5) | 0x0a: acc = (acc > e) ? acc : e; break;                 /* UMAXV */
            case (0 << 5) | 0x1a: acc = (sx(acc,esize) < sx(e,esize)) ? acc : e; break; /* SMINV */
            case (1 << 5) | 0x1a: acc = (acc < e) ? acc : e; break;                 /* UMINV */
            default: fpsimd_undef(c, insn); return;
        }
    }
    c->v[Rd].d[0] = acc & emask; c->v[Rd].d[1] = 0;
}

/* AdvSIMD two-register misc: per-element unary ops (NOT, NEG, compare-with-zero,
 * ABS, etc.) plus the common element reductions. */
static void simd_two_misc(CPU *c, u32 insn) {
    unsigned Q = BIT(30), U = BIT(29), size = BITS(23, 22);
    unsigned opc = BITS(16, 12), Rn = BITS(9, 5), Rd = BITS(4, 0);

    if (opc == 0x05 && U == 1 && size == 0) {  /* NOT (bitwise, byte) */
        c->v[Rd].d[0] = ~c->v[Rn].d[0]; c->v[Rd].d[1] = Q ? ~c->v[Rn].d[1] : 0; return;
    }
    /* REV64/REV32/REV16: reverse esize-bit elements within each container.
     * opcode 0x00 U=0 -> REV64, U=1 -> REV32; opcode 0x01 U=0 -> REV16. */
    if (opc == 0x00 || (opc == 0x01 && U == 0)) {
        unsigned container = (opc == 0x01) ? 16 : (U ? 32 : 64);
        unsigned resize = 8u << size;                       /* element size in bits */
        if (resize >= container) { fpsimd_undef(c, insn); return; }   /* arch-UNDEFINED */
        unsigned cb = container / 8, eb = resize / 8, total = Q ? 16 : 8;
        V128 rr; rr.d[0] = rr.d[1] = 0;
        for (unsigned base = 0; base < total; base += cb)
            for (unsigned e = 0; e < cb / eb; e++)
                for (unsigned k = 0; k < eb; k++)
                    rr.b[base + (cb/eb - 1 - e)*eb + k] = c->v[Rn].b[base + e*eb + k];
        c->v[Rd] = rr; return;
    }
    unsigned esize = 8u << size, n = (Q ? 16 : 8) >> size;
    u64 emask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
    V128 r; r.d[0] = r.d[1] = 0;
    for (unsigned i = 0; i < n; i++) {
        u64 a = velem_get(&c->v[Rn], size, i), v;
        switch ((U << 5) | opc) {
            case (0 << 5) | 0x08: v = (sx(a,esize) >  0) ? emask : 0; break;   /* CMGT #0 */
            case (1 << 5) | 0x08: v = (sx(a,esize) >= 0) ? emask : 0; break;   /* CMGE #0 */
            case (0 << 5) | 0x09: v = (a == 0) ? emask : 0; break;             /* CMEQ #0 */
            case (1 << 5) | 0x09: v = (sx(a,esize) <= 0) ? emask : 0; break;   /* CMLE #0 */
            case (0 << 5) | 0x0a: v = (sx(a,esize) <  0) ? emask : 0; break;   /* CMLT #0 */
            case (0 << 5) | 0x05: v = (u64)__builtin_popcount((unsigned)(a & 0xff)); break; /* CNT */
            case (0 << 5) | 0x0b: { s64 s = sx(a,esize); v = (s < 0) ? (u64)(-s) : a; } break; /* ABS */
            case (1 << 5) | 0x0b: v = (u64)(-(s64)a); break;                   /* NEG */
            default: fpsimd_undef(c, insn); return;
        }
        velem_set(&r, size, i, v & emask);
    }
    c->v[Rd] = r;
}

/* AdvSIMD scalar two-register misc — FP<->integer convert where the integer
 * operand/result sits in a SIMD register lane (not a GPR). Emitted by libc
 * number formatting, e.g. `UCVTF d,d`. Mirrors the GPR-form convert in
 * exec_fp_scalar (rounding + saturation), sourcing/writing the V128 lane. */
static void simd_scalar_cvt(CPU *c, u32 insn) {
    unsigned U = BIT(29), o2 = BIT(23), sz = BIT(22);
    unsigned opcode = BITS(16, 12), Rn = BITS(9, 5), Rd = BITS(4, 0);
    bool dbl = (sz == 1);

    if (o2 == 0 && opcode == 0x1d) {                 /* SCVTF / UCVTF: int -> fp */
        if (dbl) { u64 i = c->v[Rn].d[0]; fp_wr_d(c, Rd, U ? (double)(u64)i : (double)(s64)i); }
        else     { u32 i = c->v[Rn].s[0]; fp_wr_s(c, Rd, U ? (float)(u32)i  : (float)(s32)i);  }
        return;
    }
    if (opcode == 0x1a || opcode == 0x1b || (opcode == 0x1c && o2 == 0)) { /* FCVT* fp->int */
        double v = dbl ? fp_rd_d(c, Rn) : (double)fp_rd_s(c, Rn);
        double r;
        if      (opcode == 0x1a) r = o2 ? f_ceil(v)  : f_round(v);   /* P : N  */
        else if (opcode == 0x1b) r = o2 ? f_trunc(v) : f_floor(v);   /* Z : M  */
        else                     r = f_round(v);                    /* A      */
        u64 out;
        if (U == 0) {            /* signed, with saturation (same clamps as SCVTF block) */
            if (dbl) out = (u64)((r >= 9223372036854775807.0) ? INT64_MAX : (r <= -9223372036854775808.0) ? INT64_MIN : (s64)r);
            else     out = (u64)(u32)((r >= 2147483647.0) ? INT32_MAX : (r <= -2147483648.0) ? INT32_MIN : (s32)r);
        } else {                 /* unsigned */
            if (r < 0) r = 0;
            if (dbl) out = (r >= 18446744073709551615.0) ? UINT64_MAX : (u64)r;
            else     out = (r >= 4294967295.0) ? UINT32_MAX : (u32)r;
        }
        c->v[Rd].d[0] = out; c->v[Rd].d[1] = 0;
        return;
    }
    fpsimd_undef(c, insn);
}

/* AdvSIMD shift by immediate: SHL/SSHR/USHR (same width) and SSHLL/USHLL
 * (widening long). Used by musl/busybox string and math routines. */
static void simd_shift_imm(CPU *c, u32 insn) {
    unsigned Q = BIT(30), U = BIT(29), immh = BITS(22, 19), immb = BITS(18, 16);
    unsigned opc = BITS(15, 11), Rn = BITS(9, 5), Rd = BITS(4, 0);
    unsigned immhb = (immh << 3) | immb;
    unsigned size = (immh & 8) ? 3 : (immh & 4) ? 2 : (immh & 2) ? 1 : 0;
    unsigned esize = 8u << size;

    if (opc == 0x14) {                         /* SSHLL/USHLL (widening) */
        unsigned shift = immhb - esize, n = 64 / esize, base = Q ? n : 0;
        V128 r; r.d[0] = r.d[1] = 0;
        for (unsigned i = 0; i < n; i++) {
            u64 s = velem_get(&c->v[Rn], size, base + i);
            u64 w = U ? (s << shift) : ((u64)sx(s, esize) << shift);
            velem_set(&r, size + 1, i, w);     /* result element is 2*esize */
        }
        c->v[Rd] = r;
        return;
    }

    unsigned n = (Q ? 16 : 8) >> size;
    u64 emask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
    V128 r; r.d[0] = r.d[1] = 0;
    for (unsigned i = 0; i < n; i++) {
        u64 a = velem_get(&c->v[Rn], size, i), v;
        switch ((U << 5) | opc) {
            case (0 << 5) | 0x0a: v = a << (immhb - esize); break;             /* SHL */
            case (0 << 5) | 0x00: v = (u64)(sx(a, esize) >> (2 * esize - immhb)); break; /* SSHR */
            case (1 << 5) | 0x00: v = (a & emask) >> (2 * esize - immhb); break; /* USHR */
            default: fpsimd_undef(c, insn); return;
        }
        velem_set(&r, size, i, v & emask);
    }
    c->v[Rd] = r;
}

/* AdvSIMD scalar shift by immediate — the 64-bit D-form SHL / SSHR / USHR.
 * Emitted by libc float formatting (e.g. `SHL d,d,#52` to assemble a mantissa).
 * Only esize=64 is defined for these scalar same-width shifts (immh top bit set). */
static void simd_scalar_shift(CPU *c, u32 insn) {
    unsigned U = BIT(29), immh = BITS(22, 19), immb = BITS(18, 16);
    unsigned opc = BITS(15, 11), Rn = BITS(9, 5), Rd = BITS(4, 0);
    unsigned immhb = (immh << 3) | immb;
    if (!(immh & 8)) { fpsimd_undef(c, insn); return; }   /* scalar SHL/SHR are 64-bit */
    u64 a = c->v[Rn].d[0], v;
    switch ((U << 5) | opc) {
        case (0 << 5) | 0x0a: v = a << (immhb - 64); break;                 /* SHL  */
        case (0 << 5) | 0x00: { unsigned sh = 128 - immhb;                  /* SSHR */
            v = (u64)((s64)a >> (sh >= 64 ? 63 : sh)); } break;
        case (1 << 5) | 0x00: { unsigned sh = 128 - immhb;                  /* USHR */
            v = (sh >= 64) ? 0 : (a >> sh); } break;
        default: fpsimd_undef(c, insn); return;
    }
    c->v[Rd].d[0] = v; c->v[Rd].d[1] = 0;
}

/* ===================== ARMv8 Cryptographic Extensions =====================
 * SHA-1, SHA-256 and AES, transcribed from the ARM ARM shared pseudocode
 * (cross-checked against QEMU target/arm/tcg/crypto_helper.c).  All work on the
 * little-endian V128 lane views: pseudocode Elem[X,e,32] == X.s[e]. */

/* ror32() is provided by types.h. */
static inline u32 rol32(u32 x, unsigned n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }

static u32 sha_ch(u32 x, u32 y, u32 z)  { return (x & y) ^ (~x & z); }
static u32 sha_maj(u32 x, u32 y, u32 z) { return (x & y) ^ (x & z) ^ (y & z); }
static u32 sha_par(u32 x, u32 y, u32 z) { return x ^ y ^ z; }
static u32 sha256_bsig0(u32 x) { return ror32(x,2)  ^ ror32(x,13) ^ ror32(x,22); }
static u32 sha256_bsig1(u32 x) { return ror32(x,6)  ^ ror32(x,11) ^ ror32(x,25); }
static u32 sha256_ssig0(u32 x) { return ror32(x,7)  ^ ror32(x,18) ^ (x >> 3); }
static u32 sha256_ssig1(u32 x) { return ror32(x,17) ^ ror32(x,19) ^ (x >> 10); }

/* Sha256hash(): 4 rounds with the <y,x> = ROL(y:x,32) one-word rotation. */
static V128 sha256_hash(V128 x, V128 y, V128 w, int part1) {
    for (int e = 0; e < 4; e++) {
        u32 chs = sha_ch(y.s[0], y.s[1], y.s[2]);
        u32 maj = sha_maj(x.s[0], x.s[1], x.s[2]);
        u32 t   = y.s[3] + sha256_bsig1(y.s[0]) + chs + w.s[e];
        x.s[3]  = t + x.s[3];
        y.s[3]  = t + sha256_bsig0(x.s[0]) + maj;
        u32 ny0 = x.s[3], ny1 = y.s[0], ny2 = y.s[1], ny3 = y.s[2];
        u32 nx0 = y.s[3], nx1 = x.s[0], nx2 = x.s[1], nx3 = x.s[2];
        x.s[0]=nx0; x.s[1]=nx1; x.s[2]=nx2; x.s[3]=nx3;
        y.s[0]=ny0; y.s[1]=ny1; y.s[2]=ny2; y.s[3]=ny3;
    }
    return part1 ? x : y;
}
static V128 sha256_su0(V128 x, V128 y) {
    u32 T[4] = { x.s[1], x.s[2], x.s[3], y.s[0] };
    V128 r;
    for (int e = 0; e < 4; e++) r.s[e] = sha256_ssig0(T[e]) + x.s[e];
    return r;
}
static V128 sha256_su1(V128 x, V128 y, V128 z) {
    u32 T3[4] = { y.s[1], y.s[2], y.s[3], z.s[0] };
    V128 r;
    r.s[0] = sha256_ssig1(z.s[2]) + x.s[0] + T3[0];
    r.s[1] = sha256_ssig1(z.s[3]) + x.s[1] + T3[1];
    r.s[2] = sha256_ssig1(r.s[0]) + x.s[2] + T3[2];
    r.s[3] = sha256_ssig1(r.s[1]) + x.s[3] + T3[3];
    return r;
}

/* Cryptographic 3-register SHA: SHA1C/P/M/SU0 (op 0-3), SHA256H/H2/SU1 (op 4-6). */
static void crypto_sha3(CPU *c, u32 insn) {
    unsigned op = BITS(14, 12), Rm = BITS(20, 16), Rn = BITS(9, 5), Rd = BITS(4, 0);
    V128 d = c->v[Rd], n = c->v[Rn], m = c->v[Rm];
    switch (op) {
        case 0: case 1: case 2: {                /* SHA1C / SHA1P / SHA1M */
            for (int i = 0; i < 4; i++) {
                u32 t = (op == 0) ? sha_ch (d.s[1], d.s[2], d.s[3])
                      : (op == 1) ? sha_par(d.s[1], d.s[2], d.s[3])
                                  : sha_maj(d.s[1], d.s[2], d.s[3]);
                t += rol32(d.s[0], 5) + n.s[0] + m.s[i];
                n.s[0] = d.s[3];
                d.s[3] = d.s[2];
                d.s[2] = rol32(d.s[1], 30);
                d.s[1] = d.s[0];
                d.s[0] = t;
            }
            c->v[Rd] = d; return;
        }
        case 3:                                  /* SHA1SU0 Vd,Vn,Vm */
            d.d[0] ^= d.d[1] ^ m.d[0];
            d.d[1] ^= n.d[0] ^ m.d[1];
            c->v[Rd] = d; return;
        case 4: c->v[Rd] = sha256_hash(d, n, m, 1); return;   /* SHA256H  */
        case 5: c->v[Rd] = sha256_hash(n, d, m, 0); return;   /* SHA256H2 */
        case 6: c->v[Rd] = sha256_su1(d, n, m);     return;   /* SHA256SU1 */
        default: fpsimd_undef(c, insn); return;
    }
}

/* Cryptographic 2-register SHA: SHA1H (0), SHA1SU1 (1), SHA256SU0 (2). */
static void crypto_sha2(CPU *c, u32 insn) {
    unsigned op = BITS(16, 12), Rn = BITS(9, 5), Rd = BITS(4, 0);
    V128 d = c->v[Rd], n = c->v[Rn];
    switch (op) {
        case 0: {                                /* SHA1H Sd,Sn = ROL(Sn,30) */
            V128 r; r.d[0] = r.d[1] = 0; r.s[0] = rol32(n.s[0], 30);
            c->v[Rd] = r; return;
        }
        case 1:                                  /* SHA1SU1 Vd,Vn */
            d.s[0] = rol32(d.s[0] ^ n.s[1], 1);
            d.s[1] = rol32(d.s[1] ^ n.s[2], 1);
            d.s[2] = rol32(d.s[2] ^ n.s[3], 1);
            d.s[3] = rol32(d.s[3] ^ d.s[0], 1);
            c->v[Rd] = d; return;
        case 2: c->v[Rd] = sha256_su0(d, n); return;   /* SHA256SU0 Vd,Vn */
        default: fpsimd_undef(c, insn); return;
    }
}

static const u8 aes_sbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
static const u8 aes_inv_sbox[256] = {
  0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
  0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
  0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
  0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
  0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
  0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
  0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
  0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
  0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
  0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
  0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
  0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
  0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
  0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
  0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
  0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};
/* ShiftRows / InvShiftRows byte permutations (output[i] = input[idx[i]]). */
static const u8 aes_shift[16]     = { 0,5,10,15,4,9,14,3,8,13,2,7,12,1,6,11 };
static const u8 aes_inv_shift[16] = { 0,13,10,7,4,1,14,11,8,5,2,15,12,9,6,3 };

static u8 aes_gfmul(u8 a, u8 b) {            /* GF(2^8) multiply, poly 0x11b */
    u8 p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        u8 hi = a & 0x80;
        a = (u8)(a << 1);
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}
static void aes_mixcols(const V128 *in, V128 *out, int inv) {
    for (int col = 0; col < 4; col++) {
        const u8 *a = &in->b[col*4];
        u8 *o = &out->b[col*4];
        if (!inv) {
            o[0] = aes_gfmul(a[0],2) ^ aes_gfmul(a[1],3) ^ a[2] ^ a[3];
            o[1] = a[0] ^ aes_gfmul(a[1],2) ^ aes_gfmul(a[2],3) ^ a[3];
            o[2] = a[0] ^ a[1] ^ aes_gfmul(a[2],2) ^ aes_gfmul(a[3],3);
            o[3] = aes_gfmul(a[0],3) ^ a[1] ^ a[2] ^ aes_gfmul(a[3],2);
        } else {
            o[0] = aes_gfmul(a[0],14) ^ aes_gfmul(a[1],11) ^ aes_gfmul(a[2],13) ^ aes_gfmul(a[3],9);
            o[1] = aes_gfmul(a[0],9)  ^ aes_gfmul(a[1],14) ^ aes_gfmul(a[2],11) ^ aes_gfmul(a[3],13);
            o[2] = aes_gfmul(a[0],13) ^ aes_gfmul(a[1],9)  ^ aes_gfmul(a[2],14) ^ aes_gfmul(a[3],11);
            o[3] = aes_gfmul(a[0],11) ^ aes_gfmul(a[1],13) ^ aes_gfmul(a[2],9)  ^ aes_gfmul(a[3],14);
        }
    }
}

/* Cryptographic AES: AESE(4) AESD(5) AESMC(6) AESIMC(7). */
static void crypto_aes(CPU *c, u32 insn) {
    unsigned op = BITS(16, 12), Rn = BITS(9, 5), Rd = BITS(4, 0);
    V128 in = c->v[Rn], r;
    r.d[0] = r.d[1] = 0;
    switch (op) {
        case 4:                                  /* AESE: SubBytes(ShiftRows(Vd^Vn)) */
            for (int i = 0; i < 16; i++) in.b[i] = (u8)(c->v[Rd].b[i] ^ c->v[Rn].b[i]);
            for (int i = 0; i < 16; i++) r.b[i] = aes_sbox[in.b[aes_shift[i]]];
            c->v[Rd] = r; return;
        case 5:                                  /* AESD: InvSubBytes(InvShiftRows(Vd^Vn)) */
            for (int i = 0; i < 16; i++) in.b[i] = (u8)(c->v[Rd].b[i] ^ c->v[Rn].b[i]);
            for (int i = 0; i < 16; i++) r.b[i] = aes_inv_sbox[in.b[aes_inv_shift[i]]];
            c->v[Rd] = r; return;
        case 6: aes_mixcols(&in, &r, 0); c->v[Rd] = r; return;   /* AESMC  */
        case 7: aes_mixcols(&in, &r, 1); c->v[Rd] = r; return;   /* AESIMC */
        default: fpsimd_undef(c, insn); return;
    }
}

/* Carryless (polynomial, GF(2)) multiplies for PMULL/PMULL2. */
static void pmull64(u64 a, u64 b, u64 *lo, u64 *hi) {
    u64 rl = 0, rh = 0;
    for (int i = 0; i < 64; i++)
        if ((a >> i) & 1) { rl ^= b << i; if (i) rh ^= b >> (64 - i); }
    *lo = rl; *hi = rh;
}
static u16 pmull8(u8 a, u8 b) {
    u16 r = 0;
    for (int i = 0; i < 8; i++) if ((a >> i) & 1) r ^= (u16)b << i;
    return r;
}

/* PMULL/PMULL2 (AdvSIMD three-different, opcode 0b1110): polynomial multiply
 * long. size==3 -> 64x64->128 (needs FEAT_PMULL); size==0 -> 8x(8x8->16). Q
 * selects the low (PMULL) or high (PMULL2) source half. */
static void crypto_pmull(CPU *c, u32 insn) {
    unsigned Q = BIT(30), size = BITS(23, 22), Rm = BITS(20, 16);
    unsigned Rn = BITS(9, 5), Rd = BITS(4, 0);
    V128 r; r.d[0] = r.d[1] = 0;
    if (size == 3) {                         /* PMULL{2} Vd.1Q, Vn.1D, Vm.1D */
        u64 lo, hi;
        pmull64(c->v[Rn].d[Q ? 1 : 0], c->v[Rm].d[Q ? 1 : 0], &lo, &hi);
        r.d[0] = lo; r.d[1] = hi; c->v[Rd] = r; return;
    }
    if (size == 0) {                         /* PMULL{2} Vd.8H, Vn.8B, Vm.8B */
        unsigned base = Q ? 8 : 0;
        for (int j = 0; j < 8; j++) r.h[j] = pmull8(c->v[Rn].b[base + j], c->v[Rm].b[base + j]);
        c->v[Rd] = r; return;
    }
    fpsimd_undef(c, insn);
}

/* AdvSIMD TBL/TBX: byte table lookup across 1-4 consecutive table registers.
 * For each index byte in Vm: out = table[idx] if idx < 16*nregs, else 0 (TBL)
 * or the original Vd byte (TBX). */
static void simd_tbl(CPU *c, u32 insn) {
    unsigned Q = BIT(30), Rm = BITS(20, 16), nregs = BITS(14, 13) + 1, op = BIT(12);
    unsigned Rn = BITS(9, 5), Rd = BITS(4, 0);
    unsigned tbytes = nregs * 16, nbyte = Q ? 16 : 8;
    V128 idx = c->v[Rm], old = c->v[Rd];   /* snapshot (may alias table regs) */
    u8 table[64];
    for (unsigned r = 0; r < nregs; r++) {
        V128 t = c->v[(Rn + r) & 31];
        for (int b = 0; b < 16; b++) table[r*16 + b] = t.b[b];
    }
    V128 res; res.d[0] = res.d[1] = 0;
    for (unsigned i = 0; i < nbyte; i++) {
        unsigned ix = idx.b[i];
        res.b[i] = (ix < tbytes) ? table[ix] : (op ? old.b[i] : 0);
    }
    c->v[Rd] = res;
}

/* AdvSIMD permute: ZIP1/ZIP2 (opc 3/7), UZP1/UZP2 (1/5), TRN1/TRN2 (2/6). */
static void simd_permute(CPU *c, u32 insn) {
    unsigned Q = BIT(30), size = BITS(23, 22), Rm = BITS(20, 16), opc = BITS(14, 12);
    unsigned Rn = BITS(9, 5), Rd = BITS(4, 0);
    unsigned lanes = (Q ? 16u : 8u) >> size, half = lanes / 2;
    V128 n = c->v[Rn], m = c->v[Rm], r; r.d[0] = r.d[1] = 0;
    switch (opc) {
        case 1: case 5:                          /* UZP1 (even) / UZP2 (odd) */
            for (unsigned e = 0; e < lanes; e++) {
                unsigned s = 2*e + (opc == 5);
                velem_set(&r, size, e, (s < lanes) ? velem_get(&n, size, s)
                                                   : velem_get(&m, size, s - lanes));
            } break;
        case 2: case 6:                          /* TRN1 (even) / TRN2 (odd) */
            for (unsigned p = 0; p < half; p++) {
                unsigned s = 2*p + (opc == 6);
                velem_set(&r, size, 2*p,   velem_get(&n, size, s));
                velem_set(&r, size, 2*p+1, velem_get(&m, size, s));
            } break;
        case 3: case 7:                          /* ZIP1 (low) / ZIP2 (high) */
            for (unsigned p = 0; p < half; p++) {
                unsigned s = (opc == 7 ? half : 0) + p;
                velem_set(&r, size, 2*p,   velem_get(&n, size, s));
                velem_set(&r, size, 2*p+1, velem_get(&m, size, s));
            } break;
        default: fpsimd_undef(c, insn); return;
    }
    c->v[Rd] = r;
}

/* AdvSIMD EXT: concatenate Vn:Vm (Vm at low bytes, Vn at high bytes) and
 * extract nbyte bytes starting at byte offset imm4. */
static void simd_ext(CPU *c, u32 insn) {
    unsigned Q = BIT(30), Rm = BITS(20, 16);
    unsigned imm4 = BITS(14, 11), Rn = BITS(9, 5), Rd = BITS(4, 0);
    unsigned nbyte = Q ? 16 : 8;
    V128 n = c->v[Rn], m = c->v[Rm], r; r.d[0] = r.d[1] = 0;
    for (unsigned i = 0; i < nbyte; i++) {
        unsigned j = i + imm4;
        r.b[i] = (j < nbyte) ? m.b[j] : n.b[j - nbyte];
    }
    c->v[Rd] = r;
}

void exec_fpsimd(CPU *c, u32 insn) {
    /* Scalar floating-point (bit30=0 distinguishes from scalar AdvSIMD). */
    if ((insn & 0x7f000000) == 0x1e000000) { exec_fp_scalar(c, insn); return; }
    if ((insn & 0x7f000000) == 0x1f000000) { exec_fp_dp3(c, insn);    return; }

    /* AdvSIMD across-lanes reductions (ADDV/UMAXV/UMINV/...): bits[11:10]=10. */
    if (BITS(28, 24) == 0x0e && BITS(21, 17) == 0x18 && BIT(11) == 1 && BIT(10) == 0) {
        simd_across(c, insn); return;
    }
    /* AdvSIMD two-register misc (NOT/NEG/ABS/compare-with-zero): bits[11:10]=10. */
    if (BITS(28, 24) == 0x0e && BITS(21, 17) == 0x10 && BIT(11) == 1 && BIT(10) == 0) {
        simd_two_misc(c, insn); return;
    }
    /* AdvSIMD scalar two-register misc (bit30=1): scalar int<->FP converts. */
    if (BITS(28, 24) == 0x1e && BITS(21, 17) == 0x10 && BIT(11) == 1 && BIT(10) == 0) {
        simd_scalar_cvt(c, insn); return;
    }
    /* Cryptographic AES (AESE/AESD/AESMC/AESIMC). */
    if (BITS(31, 24) == 0x4e && BITS(23, 22) == 0 && BITS(21, 17) == 0x14 && BITS(11, 10) == 2) {
        crypto_aes(c, insn); return;
    }
    /* Cryptographic 3-register SHA (SHA1C/P/M/SU0, SHA256H/H2/SU1). */
    if (BITS(31, 24) == 0x5e && BITS(23, 21) == 0 && BIT(15) == 0 && BITS(11, 10) == 0) {
        crypto_sha3(c, insn); return;
    }
    /* Cryptographic 2-register SHA (SHA1H/SHA1SU1/SHA256SU0). */
    if (BITS(31, 24) == 0x5e && BITS(23, 22) == 0 && BITS(21, 17) == 0x14 && BITS(11, 10) == 2) {
        crypto_sha2(c, insn); return;
    }
    /* AdvSIMD three-different PMULL/PMULL2 (opcode 0b1110, U=0). */
    if (BITS(28, 24) == 0x0e && BIT(29) == 0 && BIT(21) == 1 && BITS(15, 12) == 0xe && BITS(11, 10) == 0) {
        crypto_pmull(c, insn); return;
    }
    /* AdvSIMD TBL/TBX (table vector lookup). */
    if (BIT(31) == 0 && BIT(29) == 0 && BITS(28, 24) == 0x0e && BITS(23, 21) == 0 &&
        BIT(15) == 0 && BITS(11, 10) == 0) {
        simd_tbl(c, insn); return;
    }
    /* AdvSIMD EXT (byte extract from concatenated pair). */
    if (BIT(31) == 0 && BIT(29) == 1 && BITS(28, 24) == 0x0e &&
        BITS(23, 22) == 0 && BIT(21) == 0 && BIT(15) == 0 && BITS(11, 10) == 0) {
        simd_ext(c, insn); return;
    }
    /* AdvSIMD permute (ZIP/UZP/TRN). */
    if (BIT(31) == 0 && BIT(29) == 0 && BITS(28, 24) == 0x0e && BIT(21) == 0 &&
        BIT(15) == 0 && BITS(11, 10) == 2) {
        simd_permute(c, insn); return;
    }
    /* AdvSIMD three-same (vector integer): ADD/SUB/CMP/MIN/MAX/MUL/logical. */
    if (BITS(28, 24) == 0x0e && BIT(21) == 1 && BIT(10) == 1) { simd_three_same(c, insn); return; }

    /* Advanced SIMD modified immediate (MOVI/MVNI/ORR/BIC/FMOV vector imm). */
    if (BITS(28, 19) == 0x1e0 && BIT(10) == 1) { simd_modified_imm(c, insn); return; }
    /* AdvSIMD shift by immediate (SHL/SSHR/USHR/SSHLL/USHLL): immh != 0. */
    if (BITS(28, 23) == 0x1e && BIT(10) == 1 && BITS(22, 19) != 0) { simd_shift_imm(c, insn); return; }
    /* AdvSIMD scalar shift by immediate (bit30=1): D-form SHL/SSHR/USHR. */
    if (BITS(28, 23) == 0x3e && BIT(10) == 1 && BITS(22, 19) != 0) { simd_scalar_shift(c, insn); return; }
    /* AdvSIMD copy (DUP/INS/UMOV/SMOV). */
    if (BITS(28, 21) == 0x70 && BIT(15) == 0 && BIT(10) == 1) { simd_copy(c, insn); return; }

    fpsimd_undef(c, insn);
}
