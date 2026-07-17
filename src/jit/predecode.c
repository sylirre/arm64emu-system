/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Pre-decode classifier: turn one instruction word into a PDEnt (dense
 * opcode id + pre-extracted operands). Runs at JIT translation time.
 *
 * The classification mirrors decode.c's tree for the covered forms and is
 * deliberately conservative: any encoding not recognized with certainty
 * (including architecturally-unallocated patterns decode.c happens to accept
 * leniently) is left as PD_GENERIC, which the translator turns into a call
 * back into exec_a64 — so behavior for those stays byte-identical by
 * construction. Ported from arm64chroot; deviations marked "coverage:". */
#include "predecode.h"

#define BIT(i)      ((insn >> (i)) & 1u)
#define BITS(hi,lo) ((insn >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

/* Duplicated from decode.c (which must stay unmodified). */
static bool pd_bitmasks(unsigned immN, unsigned imms, unsigned immr,
                        bool is64, u64 *wmask) {
    unsigned width = is64 ? 64 : 32;
    u32 nimms = ((immN & 1) << 6) | ((~imms) & 0x3f);
    if (nimms == 0) return false;
    int len = 31 - __builtin_clz(nimms);
    if (len < 1) return false;
    if ((1u << len) > width) return false;
    unsigned levels = (1u << len) - 1;
    unsigned S = imms & levels;
    unsigned R = immr & levels;
    unsigned esize = 1u << len;
    u64 welem = ones(S + 1);
    u64 emask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
    welem &= emask;
    unsigned r = R % esize;
    u64 w = r ? (((welem >> r) | (welem << (esize - r))) & emask) : welem;
    u64 wm = 0;
    for (unsigned i = 0; i < 64; i += esize) wm |= w << i;
    if (!is64) wm = (u32)wm;
    *wmask = wm;
    return true;
}

/* ---- DP immediate (top groups 0x8/0x9) ---- */
static void fill_dp_imm(PDEnt *e, u32 insn) {
    unsigned t = BITS(28, 23);
    bool sf = BIT(31);

    if (t == 0x20 || t == 0x21) {                    /* ADR / ADRP */
        u32 immlo = BITS(30, 29), immhi = BITS(23, 5);
        s64 imm = (s64)sign_extend(((u64)immhi << 2) | immlo, 21);
        if (!sf) { e->op = PD_ADR; e->imm = (u64)imm; }
        else { e->op = PD_ADRP; e->imm = (u64)imm << 12; }
        return;
    }
    if (t == 0x22) {                                 /* add/sub immediate */
        bool op = BIT(30), S = BIT(29), sh = BIT(22);
        u64 imm = BITS(21, 10);
        if (sh) imm <<= 12;
        if (S) {
            e->op = op ? (sf ? PD_SUBS64I : PD_SUBS32I)
                       : (sf ? PD_ADDS64I : PD_ADDS32I);
            e->imm = op ? ~imm : imm;   /* SUBS: pre-inverted for add-with-carry */
        } else {
            e->op = op ? (sf ? PD_SUB64I : PD_SUB32I)
                       : (sf ? PD_ADD64I : PD_ADD32I);
            e->imm = imm;
        }
        return;
    }
    if (t == 0x24) {                                 /* logical immediate */
        unsigned opc = BITS(30, 29), N = BIT(22), immr = BITS(21, 16), imms = BITS(15, 10);
        u64 wmask;
        if (!pd_bitmasks(N, imms, immr, sf, &wmask)) return;   /* GENERIC */
        static const u8 ids[4][2] = {
            { PD_AND32I, PD_AND64I }, { PD_ORR32I, PD_ORR64I },
            { PD_EOR32I, PD_EOR64I }, { PD_ANDS32I, PD_ANDS64I },
        };
        e->op = ids[opc][sf];
        e->imm = wmask;
        return;
    }
    if (t == 0x25) {                                 /* move wide */
        unsigned opc = BITS(30, 29), hw = BITS(22, 21), imm16 = BITS(20, 5);
        unsigned shift = hw * 16;
        if (opc == 0) {                              /* MOVN: precompute */
            u64 r = ~((u64)imm16 << shift);
            e->op = PD_MOVI; e->imm = sf ? r : (u32)r;
        } else if (opc == 2) {                       /* MOVZ: precompute */
            u64 r = (u64)imm16 << shift;
            e->op = PD_MOVI; e->imm = sf ? r : (u32)r;
        } else if (opc == 3) {                       /* MOVK */
            e->op = sf ? PD_MOVK64 : PD_MOVK32;
            e->rm = (u8)shift;
            e->imm = (u64)imm16 << shift;
        }
        return;
    }
    if (t == 0x26) {                                 /* bitfield aliases only */
        unsigned opc = BITS(30, 29), N = BIT(22), immr = BITS(21, 16), imms = BITS(15, 10);
        unsigned width = sf ? 64 : 32;
        if (sf ? (N != 1) : (N != 0 || immr >= 32 || imms >= 32)) return;
        if (opc == 2) {                              /* UBFM */
            if (imms == width - 1) {                 /* LSR */
                e->op = sf ? PD_LSR64I : PD_LSR32I; e->rm = (u8)immr;
            } else if (immr == imms + 1) {           /* LSL */
                e->op = sf ? PD_LSL64I : PD_LSL32I; e->rm = (u8)(width - 1 - imms);
            } else if (imms >= immr) {               /* UBFX / UXTB / UXTH */
                e->op = sf ? PD_UBFX64 : PD_UBFX32;
                e->rm = (u8)immr; e->imm = ones(imms - immr + 1);
            } else {                                 /* UBFIZ */
                e->op = sf ? PD_UBFIZ64 : PD_UBFIZ32;
                e->rm = (u8)(width - immr); e->imm = ones(imms + 1);
            }
        } else if (opc == 0) {                       /* SBFM */
            if (imms == width - 1) {                 /* ASR */
                e->op = sf ? PD_ASR64I : PD_ASR32I; e->rm = (u8)immr;
            } else if (imms >= immr) {               /* SBFX / SXTB / SXTH / SXTW */
                e->op = sf ? PD_SBFX64 : PD_SBFX32;
                e->rm = (u8)(width - 1 - imms);
                e->imm = (u64)(width - 1 - imms + immr);
            }
            /* SBFIZ: GENERIC */
        }
        /* BFM/BFI/BFXIL: GENERIC */
        return;
    }
    if (t == 0x27) {                                 /* EXTR / ROR imm */
        unsigned imms = BITS(15, 10);
        if (sf) { e->op = PD_EXTR64; e->imm = imms; }
        else if (imms < 32) { e->op = PD_EXTR32; e->imm = imms; }
        return;
    }
}

/* ---- branches / system (top groups 0xa/0xb) ---- */
static void fill_branch(PDEnt *e, u32 insn) {
    unsigned top6 = BITS(31, 26);

    if (top6 == 0x05) {
        e->op = PD_B;
        e->imm = (sign_extend(BITS(25, 0), 26) << 2);
        return;
    }
    if (top6 == 0x25) {
        e->op = PD_BL;
        e->imm = (sign_extend(BITS(25, 0), 26) << 2);
        return;
    }
    if (BITS(31, 24) == 0x54 && BIT(4) == 0) {       /* B.cond */
        e->op = PD_BCOND;
        e->rd = (u8)BITS(3, 0);
        e->imm = (sign_extend(BITS(23, 5), 19) << 2);
        return;
    }
    if (BITS(30, 25) == 0x1a) {                      /* CBZ/CBNZ */
        bool sf = BIT(31), op = BIT(24);
        e->op = op ? (sf ? PD_CBNZ64 : PD_CBNZ32) : (sf ? PD_CBZ64 : PD_CBZ32);
        e->imm = (sign_extend(BITS(23, 5), 19) << 2);
        return;
    }
    if (BITS(30, 25) == 0x1b) {                      /* TBZ/TBNZ */
        e->op = BIT(24) ? PD_TBNZ : PD_TBZ;
        e->rm = (u8)((BIT(31) << 5) | BITS(23, 19));
        e->imm = (sign_extend(BITS(18, 5), 14) << 2);
        return;
    }
    if (BITS(31, 25) == 0x6b) {                      /* BR/BLR/RET */
        unsigned opc = BITS(24, 21);
        if (opc == 0 || opc == 2) e->op = PD_BR;     /* BR and RET: pc = Xn */
        else if (opc == 1) e->op = PD_BLR;
        return;
    }
    if (BITS(31, 24) == 0xd5) {                      /* hints: NOP and friends */
        unsigned L = BIT(21), op0 = BITS(20, 19), op1 = BITS(18, 16);
        unsigned CRn = BITS(15, 12), CRm = BITS(11, 8), op2 = BITS(7, 5), Rt = BITS(4, 0);
        if (L == 0 && op0 == 0 && op1 == 3 && CRn == 2 && Rt == 31 &&
            !(CRm == 0 && (op2 == 2 || op2 == 3)))   /* WFE/WFI stay GENERIC */
            e->op = PD_NOP;
        return;
    }
}

/* ---- loads/stores (top groups 0x4/0x6/0xc/0xe) ---- */

/* Integer single register: id for (size, opc), or 0 if not covered. */
static u8 ldst_int_id(unsigned size, unsigned opc) {
    switch ((size << 2) | opc) {
        case 0x0: return PD_STRB;    case 0x1: return PD_LDRB;
        case 0x2: return PD_LDRSB64; case 0x3: return PD_LDRSB32;
        case 0x4: return PD_STRH;    case 0x5: return PD_LDRH;
        case 0x6: return PD_LDRSH64; case 0x7: return PD_LDRSH32;
        case 0x8: return PD_STR32U;  case 0x9: return PD_LDR32U;
        case 0xa: return PD_LDRSW;
        case 0xc: return PD_STR64U;  case 0xd: return PD_LDR64U;
        case 0xe: return PD_NOP;     /* PRFM: no access, no register write */
        default:  return 0;
    }
}

static void fill_ldst(PDEnt *e, u32 insn) {
    unsigned b2927 = BITS(29, 27);

    if (b2927 == 0x3 && BITS(25, 24) == 0) {         /* literal */
        unsigned opc = BITS(31, 30);
        if (BIT(26)) return;                         /* coverage: SIMD&FP literal
                                                      * is UNDEF in decode.c
                                                      * (ldst_literal V=1) ->
                                                      * GENERIC, not PD_LDRLITV */
        if (opc == 0) e->op = PD_LDRLIT32;
        else if (opc == 1) e->op = PD_LDRLIT64;
        else if (opc == 3) e->op = PD_NOP;           /* PRFM literal */
        e->imm = (sign_extend(BITS(23, 5), 19) << 2);
        return;
    }

    if (b2927 == 0x5) {                              /* pairs */
        unsigned opc = BITS(31, 30), mode = BITS(25, 23);
        bool V = BIT(26), L = BIT(22);
        if (mode > 3) return;
        /* mode: 0 = STNP/LDNP (plain offset), 1 = post, 2 = offset, 3 = pre */
        unsigned kind = (mode == 1) ? 2 : (mode == 3) ? 1 : 0;  /* 0 off, 1 pre, 2 post */
        unsigned scale;
        u8 base_id;
        if (V) {
            if (opc == 2)      { scale = 4; base_id = L ? PD_LDPQ : PD_STPQ; }
            else if (opc == 1) { scale = 3; base_id = L ? PD_LDPD : PD_STPD; }
            else return;                             /* S pairs / opc==3: GENERIC */
        } else {
            if (opc == 2)      { scale = 3; base_id = L ? PD_LDP64 : PD_STP64; }
            else if (opc == 0) { scale = 2; base_id = L ? PD_LDP32 : PD_STP32; }
            else return;                             /* LDPSW / opc==3: GENERIC */
        }
        e->op = (u8)(base_id + kind);                /* relies on OFF,PRE,POST id order */
        e->rm = (u8)BITS(14, 10);                    /* Rt2 */
        e->imm = (sign_extend(BITS(21, 15), 7) << scale);
        return;
    }

    if (b2927 == 0x7) {                              /* single register */
        unsigned size = BITS(31, 30), opc = BITS(23, 22);
        bool V = BIT(26);

        if (BIT(24)) {                               /* unsigned immediate offset */
            if (V) {
                if (opc & 2) {                       /* Q form (requires size 0) */
                    if (size != 0) return;
                    e->op = (opc & 1) ? PD_LDRQ : PD_STRQ;
                    e->imm = (u64)BITS(21, 10) << 4;
                } else {
                    e->op = (opc & 1) ? PD_LDRV : PD_STRV;
                    e->rm = (u8)(1u << size);
                    e->imm = (u64)BITS(21, 10) << size;
                }
            } else {
                u8 id = ldst_int_id(size, opc);
                if (!id) return;
                e->op = id;
                e->imm = (u64)BITS(21, 10) << size;
            }
            return;
        }
        if (BIT(21)) {                               /* register offset (or atomics) */
            if (BITS(11, 10) != 2 || V) return;      /* atomics/other: GENERIC */
            u8 id;
            switch ((size << 2) | opc) {
                case 0x0: id = PD_STRBRO;  break; case 0x1: id = PD_LDRBRO;  break;
                case 0x4: id = PD_STRHRO;  break; case 0x5: id = PD_LDRHRO;  break;
                case 0x8: id = PD_STR32RO; break; case 0x9: id = PD_LDR32RO; break;
                case 0xc: id = PD_STR64RO; break; case 0xd: id = PD_LDR64RO; break;
                default: return;                     /* LDRS* reg-offset: GENERIC */
            }
            e->op = id;
            e->imm = (u64)(BITS(15, 13) << 3) | (BIT(12) ? size : 0);
            return;
        }
        {                                            /* imm9 addressing modes */
            unsigned mode = BITS(11, 10);
            u64 imm9 = (u64)(s64)sign_extend(BITS(20, 12), 9);
            if (mode == 0 || mode == 2) {            /* LDUR/STUR (+unpriv): plain
                                                      * (decode.c also runs LDTR/
                                                      * STTR as plain EL-priv) */
                if (V) {
                    if (opc & 2) {
                        if (size != 0) return;
                        e->op = (opc & 1) ? PD_LDRQ : PD_STRQ;
                    } else {
                        e->op = (opc & 1) ? PD_LDRV : PD_STRV;
                        e->rm = (u8)(1u << size);
                    }
                } else {
                    u8 id = ldst_int_id(size, opc);
                    if (!id) return;
                    e->op = id;
                }
                e->imm = imm9;
                return;
            }
            /* mode 1 = post-index, mode 3 = pre-index (writeback) */
            bool post = (mode == 1);
            if (V) {
                if ((opc & 2) && size == 0) {        /* Q with writeback */
                    e->op = (opc & 1) ? (post ? PD_LDRQPOST : PD_LDRQPRE)
                                      : (post ? PD_STRQPOST : PD_STRQPRE);
                    e->imm = imm9;
                }
                return;
            }
            if (size == 3 && opc == 1) e->op = post ? PD_LDR64POST : PD_LDR64PRE;
            else if (size == 3 && opc == 0) e->op = post ? PD_STR64POST : PD_STR64PRE;
            else if (size == 2 && opc == 1) e->op = post ? PD_LDR32POST : PD_LDR32PRE;
            else if (size == 2 && opc == 0) e->op = post ? PD_STR32POST : PD_STR32PRE;
            else if (size == 0 && opc == 1 && post) e->op = PD_LDRBPOST;
            else if (size == 0 && opc == 0 && post) e->op = PD_STRBPOST;
            else return;
            e->imm = imm9;
            return;
        }
    }
    /* exclusives, LSE atomics, vector structure loads: GENERIC */
}

/* ---- DP register (top groups 0x5/0xd) ---- */
static void fill_dp_reg(PDEnt *e, u32 insn) {
    bool sf = BIT(31);
    unsigned op24 = BITS(28, 24);

    if (op24 == 0x0a) {                              /* logical shifted register */
        unsigned opc = BITS(30, 29), shift = BITS(23, 22), N = BIT(21), imm6 = BITS(15, 10);
        if (!sf && imm6 >= 32) return;
        if (shift == 0 && imm6 == 0) {
            static const u8 ids[4][2][2] = {         /* [opc][N][sf] */
                { { PD_AND32,  PD_AND64  }, { PD_BIC32,  PD_BIC64  } },
                { { PD_ORR32,  PD_ORR64  }, { PD_ORN32,  PD_ORN64  } },
                { { PD_EOR32,  PD_EOR64  }, { PD_EON32,  PD_EON64  } },
                { { PD_ANDS32, PD_ANDS64 }, { PD_BICS32, PD_BICS64 } },
            };
            e->op = ids[opc][N][sf];
        } else {
            static const u8 ids[4][2] = {            /* [opc][sf] */
                { PD_AND32S, PD_AND64S }, { PD_ORR32S, PD_ORR64S },
                { PD_EOR32S, PD_EOR64S }, { PD_ANDS32S, PD_ANDS64S },
            };
            e->op = ids[opc][sf];
            e->imm = ((u64)N << 8) | (shift << 6) | imm6;
        }
        return;
    }
    if (op24 == 0x0b) {                              /* add/sub register */
        bool op = BIT(30), S = BIT(29);
        if (BIT(21)) {                               /* extended register */
            static const u8 ids[2][2][2] = {         /* [op][S][sf] */
                { { PD_ADDX32, PD_ADDX64 }, { PD_ADDSX32, PD_ADDSX64 } },
                { { PD_SUBX32, PD_SUBX64 }, { PD_SUBSX32, PD_SUBSX64 } },
            };
            e->op = ids[op][S][sf];
            e->imm = (BITS(15, 13) << 3) | BITS(12, 10);
        } else {                                     /* shifted register */
            unsigned shift = BITS(23, 22), imm6 = BITS(15, 10);
            if (shift == 3) return;                  /* ROR: unallocated here */
            if (!sf && imm6 >= 32) return;
            if (shift == 0 && imm6 == 0) {
                static const u8 ids[2][2][2] = {     /* [op][S][sf] */
                    { { PD_ADD32R, PD_ADD64R }, { PD_ADDS32R, PD_ADDS64R } },
                    { { PD_SUB32R, PD_SUB64R }, { PD_SUBS32R, PD_SUBS64R } },
                };
                e->op = ids[op][S][sf];
            } else {
                static const u8 ids[2][2][2] = {
                    { { PD_ADD32RS, PD_ADD64RS }, { PD_ADDS32RS, PD_ADDS64RS } },
                    { { PD_SUB32RS, PD_SUB64RS }, { PD_SUBS32RS, PD_SUBS64RS } },
                };
                e->op = ids[op][S][sf];
                e->imm = (shift << 6) | imm6;
            }
        }
        return;
    }
    if (op24 == 0x1b) {                              /* 3-source */
        unsigned key = (BITS(23, 21) << 1) | BIT(15);
        e->imm = BITS(14, 10);                       /* Ra */
        switch (key) {
            case 0x0: e->op = sf ? PD_MADD64 : PD_MADD32; break;
            case 0x1: e->op = sf ? PD_MSUB64 : PD_MSUB32; break;
            case 0x2: if (sf) e->op = PD_SMADDL; break;
            case 0x3: if (sf) e->op = PD_SMSUBL; break;
            case 0x4: if (sf) e->op = PD_SMULH; break;
            case 0xa: if (sf) e->op = PD_UMADDL; break;
            case 0xb: if (sf) e->op = PD_UMSUBL; break;
            case 0xc: if (sf) e->op = PD_UMULH; break;
            default: break;
        }
        if (e->op == PD_GENERIC) e->imm = 0;
        return;
    }
    if (op24 == 0x1a) {
        unsigned op21 = BITS(28, 21);
        if (op21 == 0xd2) {                          /* CCMP/CCMN */
            bool op = BIT(30), is_imm = BIT(11);
            unsigned nzcv = BITS(3, 0);
            u32 flags = ((nzcv & 8) ? PS_N : 0) | ((nzcv & 4) ? PS_Z : 0) |
                        ((nzcv & 2) ? PS_C : 0) | ((nzcv & 1) ? PS_V : 0);
            static const u8 ids[2][2][2] = {         /* [op][is_imm][sf] */
                { { PD_CCMN32R, PD_CCMN64R }, { PD_CCMN32I, PD_CCMN64I } },
                { { PD_CCMP32R, PD_CCMP64R }, { PD_CCMP32I, PD_CCMP64I } },
            };
            e->op = ids[op][is_imm][sf];
            e->imm = BITS(15, 12) | ((u64)BITS(20, 16) << 8) | ((u64)flags << 32);
            return;
        }
        if (op21 == 0xd4) {                          /* CSEL family */
            bool op = BIT(30), o2 = BIT(10);
            static const u8 ids[2][2][2] = {         /* [op][o2][sf] */
                { { PD_CSEL32, PD_CSEL64 }, { PD_CSINC32, PD_CSINC64 } },
                { { PD_CSINV32, PD_CSINV64 }, { PD_CSNEG32, PD_CSNEG64 } },
            };
            e->op = ids[op][o2][sf];
            e->imm = BITS(15, 12);                   /* cond */
            return;
        }
        if (op21 == 0xd6) {
            unsigned opcode = BITS(15, 10);
            if (BIT(30)) {                           /* 1-source (hot subset) */
                if (opcode == 0x02 && !sf) e->op = PD_REVW;
                else if (opcode == 0x03 && sf) e->op = PD_REV64;
                else if (opcode == 0x04) e->op = sf ? PD_CLZ64 : PD_CLZ32;
            } else {                                 /* 2-source */
                switch (opcode) {
                    case 0x02: e->op = sf ? PD_UDIV64 : PD_UDIV32; break;
                    case 0x03: e->op = sf ? PD_SDIV64 : PD_SDIV32; break;
                    case 0x08: e->op = sf ? PD_LSLV64 : PD_LSLV32; break;
                    case 0x09: e->op = sf ? PD_LSRV64 : PD_LSRV32; break;
                    case 0x0a: e->op = sf ? PD_ASRV64 : PD_ASRV32; break;
                    case 0x0b: e->op = sf ? PD_RORV64 : PD_RORV32; break;
                    default: break;                  /* CRC32: GENERIC */
                }
            }
            return;
        }
        /* ADC/SBC (0xd0): GENERIC */
    }
}

void pd_fill(PDEnt *e, u32 insn) {
    e->insn = insn;
    e->op = PD_GENERIC;
    e->rd = (u8)(insn & 31);
    e->rn = (u8)((insn >> 5) & 31);
    e->rm = (u8)((insn >> 16) & 31);
    e->imm = 0;
    switch ((insn >> 25) & 0xf) {
        case 0x8: case 0x9: fill_dp_imm(e, insn); break;
        case 0xa: case 0xb: fill_branch(e, insn); break;
        case 0x4: case 0x6: case 0xc: case 0xe: fill_ldst(e, insn); break;
        case 0x5: case 0xd: fill_dp_reg(e, insn); break;
        default: break;
    }
}
