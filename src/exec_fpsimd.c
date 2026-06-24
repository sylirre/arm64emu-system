/* AArch64 FP/Advanced-SIMD execution. Grown on demand as the firmware/kernel
 * exercise instructions (the "implement on demand" M4 strategy). */
#include "cpu.h"
#include "esr.h"
#include <stdio.h>

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

void exec_fpsimd(CPU *c, u32 insn) {
    /* Advanced SIMD modified immediate (MOVI/MVNI/ORR/BIC/FMOV vector imm). */
    if (BITS(28, 19) == 0x1e0 && BIT(10) == 1) { simd_modified_imm(c, insn); return; }
    /* AdvSIMD copy (DUP/INS/UMOV/SMOV). */
    if (BITS(28, 21) == 0x70 && BIT(15) == 0 && BIT(10) == 1) { simd_copy(c, insn); return; }

    fpsimd_undef(c, insn);
}
