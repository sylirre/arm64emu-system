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
                case 0x4: if (!dbl) fp_wr_d(c, Rd, (double)fp_rd_s(c, Rn)); else fpsimd_undef(c, insn); return; /* FCVT S->D */
                case 0x5: if (dbl) fp_wr_s(c, Rd, (float)fp_rd_d(c, Rn)); else fpsimd_undef(c, insn); return;  /* FCVT D->S */
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

    if (opc == 0x17 && U == 0) {               /* ADDP (pairwise add) */
        for (unsigned i = 0; i < n / 2; i++) {
            u64 lo = velem_get(&c->v[Rn], size, 2 * i) + velem_get(&c->v[Rn], size, 2 * i + 1);
            u64 hi = velem_get(&c->v[Rm], size, 2 * i) + velem_get(&c->v[Rm], size, 2 * i + 1);
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
