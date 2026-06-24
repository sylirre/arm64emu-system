/* A64 instruction decoder/executor — integer/branch/load-store (M1).
 * FP/SIMD lives in exec_fpsimd.c (M4). System regs in sysreg.c (M2). */
#include "cpu.h"
#include <string.h>
#include "mmu.h"
#include "esr.h"
#include "sysreg.h"
#include <stdio.h>

/* FP/SIMD group hook (M4). Weak so M1 links standalone. */
void exec_fpsimd(CPU *c, u32 insn) __attribute__((weak));

static void undefined(CPU *c, u32 insn) {
    /* Architecturally an UNDEFINED instruction takes a synchronous exception
     * with EC_UNKNOWN. During bring-up we also log it. */
    if (g_trace)
        fprintf(stderr, "UNDEF insn 0x%08x at pc=0x%llx\n",
                insn, (unsigned long long)c->cur_insn_pc);
    cpu_raise_sync(c, esr_make(EC_UNKNOWN, 0), 0);
}

/* ---------- arithmetic helpers ---------- */

static u64 add_with_carry(u64 x, u64 y, int cin, bool is64, u32 *flags) {
    u64 res;
    u32 N, Z, C, V;
    if (is64) {
        unsigned __int128 u = (unsigned __int128)x + (unsigned __int128)y + (unsigned)cin;
        res = (u64)u;
        C = (u32)((u >> 64) & 1);
        __int128 s = (__int128)(s64)x + (__int128)(s64)y + cin;
        V = ((__int128)(s64)res != s);
        N = (u32)(res >> 63);
        Z = (res == 0);
    } else {
        u32 xx = (u32)x, yy = (u32)y;
        u64 u = (u64)xx + (u64)yy + (unsigned)cin;
        res = (u32)u;
        C = (u32)((u >> 32) & 1);
        s64 s = (s64)(s32)xx + (s64)(s32)yy + cin;
        V = ((s64)(s32)(u32)res != s);
        N = (u32)((u32)res >> 31);
        Z = ((u32)res == 0);
    }
    if (flags) *flags = (N ? PS_N : 0) | (Z ? PS_Z : 0) | (C ? PS_C : 0) | (V ? PS_V : 0);
    return res;
}

static void set_logical_flags(CPU *c, u64 res, bool is64) {
    u32 f = 0;
    if (is64) { if (res >> 63) f |= PS_N; if (res == 0) f |= PS_Z; }
    else { u32 r = (u32)res; if (r >> 31) f |= PS_N; if (r == 0) f |= PS_Z; }
    c->nzcv = f;   /* C=V=0 */
}

static u64 shift_reg(u64 v, unsigned type, unsigned amount, bool is64) {
    unsigned w = is64 ? 64 : 32;
    amount &= (w - 1);
    if (!is64) v = (u32)v;
    switch (type) {
        case 0: return v << amount;                                  /* LSL */
        case 1: return is64 ? (v >> amount) : ((u32)v >> amount);    /* LSR */
        case 2: return is64 ? (u64)((s64)v >> amount)               /* ASR */
                            : (u64)(u32)((s32)(u32)v >> amount);
        default: return is64 ? ror64(v, amount) : ror32((u32)v, amount); /* ROR */
    }
}

/* ARMv8 CRC32/CRC32C: bit-reflected CRC over `bytes` low bytes of `data`,
 * accumulator `acc`. poly is the reflected polynomial (0xEDB88320 for CRC32,
 * 0x82F63B78 for CRC32C). Matches the hardware instruction the kernel uses. */
static u32 crc32_step(u32 acc, u64 data, unsigned bytes, u32 poly) {
    for (unsigned i = 0; i < bytes; i++) {
        acc ^= (u32)((data >> (8 * i)) & 0xff);
        for (int k = 0; k < 8; k++)
            acc = (acc >> 1) ^ (poly & (u32)(-(s32)(acc & 1)));
    }
    return acc;
}

static u64 extend_reg(u64 v, unsigned option, unsigned shift) {
    u64 out;
    switch (option & 0x3) {
        case 0: out = (u8)v;  break;
        case 1: out = (u16)v; break;
        case 2: out = (u32)v; break;
        default: out = v;     break;
    }
    if (option & 0x4) {  /* signed */
        unsigned bits = (option & 3) == 0 ? 8 : (option & 3) == 1 ? 16 : (option & 3) == 2 ? 32 : 64;
        out = sign_extend(out, bits);
    }
    return out << shift;
}

static u64 ror_within(u64 v, unsigned r, unsigned width) {
    if (width == 64) return ror64(v, r);
    v = (u32)v; r %= width;
    return r ? ((v >> r) | (v << (width - r))) & 0xffffffffULL : v;
}

static bool decode_bitmasks(unsigned immN, unsigned imms, unsigned immr,
                            bool is64, u64 *wmask, u64 *tmask) {
    unsigned width = is64 ? 64 : 32;
    u32 nimms = ((immN & 1) << 6) | ((~imms) & 0x3f);
    if (nimms == 0) return false;
    int len = 31 - __builtin_clz(nimms);
    if (len < 1) return false;
    if ((1u << len) > width) return false;
    unsigned levels = (1u << len) - 1;
    unsigned S = imms & levels;
    unsigned R = immr & levels;
    unsigned diff = (S - R) & levels;
    unsigned esize = 1u << len;
    u64 welem = ones(S + 1);
    u64 telem = ones(diff + 1);
    u64 emask = (esize == 64) ? ~0ULL : ((1ULL << esize) - 1);
    welem &= emask; telem &= emask;
    unsigned r = R % esize;
    u64 w = r ? (((welem >> r) | (welem << (esize - r))) & emask) : welem;
    u64 wm = 0, tm = 0;
    for (unsigned i = 0; i < 64; i += esize) { wm |= w << i; tm |= telem << i; }
    if (!is64) { wm = (u32)wm; tm = (u32)tm; }
    if (wmask) *wmask = wm;
    if (tmask) *tmask = tm;
    return true;
}

/* ---------- field extraction ---------- */
#define BIT(i)      ((insn >> (i)) & 1u)
#define BITS(hi,lo) ((insn >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

/* ================= DP-immediate ================= */
static void dp_immediate(CPU *c, u32 insn) {
    unsigned t = BITS(28, 23);
    bool sf = BIT(31);
    unsigned Rd = BITS(4, 0), Rn = BITS(9, 5);

    if (t == 0x20 || t == 0x21) {                 /* PC-rel: ADR/ADRP */
        u32 immlo = BITS(30, 29), immhi = BITS(23, 5);
        s64 imm = (s64)sign_extend(((u64)immhi << 2) | immlo, 21);
        if (BIT(31) == 0) set_x(c, Rd, c->cur_insn_pc + imm);
        else set_x(c, Rd, (c->cur_insn_pc & ~0xfffULL) + ((u64)imm << 12));
        return;
    }
    if (t == 0x22) {                              /* add/sub immediate */
        bool op = BIT(30), S = BIT(29), sh = BIT(22);
        u64 imm = BITS(21, 10);
        if (sh) imm <<= 12;
        u64 n = reg_xsp(c, Rn);
        u32 fl;
        u64 r = op ? add_with_carry(n, ~imm, 1, sf, &fl)
                   : add_with_carry(n, imm, 0, sf, &fl);
        if (S) { c->nzcv = fl; set_x_sz(c, Rd, sf, r); }
        else set_xsp(c, Rd, sf ? r : (u32)r);
        return;
    }
    if (t == 0x24) {                              /* logical immediate */
        unsigned opc = BITS(30, 29), N = BIT(22), immr = BITS(21, 16), imms = BITS(15, 10);
        u64 wmask;
        if (!decode_bitmasks(N, imms, immr, sf, &wmask, NULL)) { undefined(c, insn); return; }
        u64 n = reg_x(c, Rn), r;
        switch (opc) {
            case 0: r = n & wmask; break;
            case 1: r = n | wmask; break;
            case 2: r = n ^ wmask; break;
            default: r = n & wmask; break;
        }
        if (!sf) r = (u32)r;
        if (opc == 3) { set_logical_flags(c, r, sf); set_x(c, Rd, r); }
        else set_xsp(c, Rd, r);
        return;
    }
    if (t == 0x25) {                              /* move wide immediate */
        unsigned opc = BITS(30, 29), hw = BITS(22, 21), imm16 = BITS(20, 5);
        unsigned shift = hw * 16;
        u64 r;
        if (opc == 0) r = ~((u64)imm16 << shift);             /* MOVN */
        else if (opc == 2) r = (u64)imm16 << shift;           /* MOVZ */
        else if (opc == 3) {                                  /* MOVK */
            u64 cur = reg_x(c, Rd);
            r = (cur & ~((u64)0xffff << shift)) | ((u64)imm16 << shift);
        } else { undefined(c, insn); return; }
        set_x_sz(c, Rd, sf, r);
        return;
    }
    if (t == 0x26) {                              /* bitfield SBFM/BFM/UBFM */
        unsigned opc = BITS(30, 29), N = BIT(22), immr = BITS(21, 16), imms = BITS(15, 10);
        u64 wmask, tmask;
        if (!decode_bitmasks(N, imms, immr, sf, &wmask, &tmask)) { undefined(c, insn); return; }
        u64 src = reg_x(c, Rn);
        u64 ror = ror_within(src, immr, sf ? 64 : 32);
        u64 bot = (opc == 1) ? ((reg_x(c, Rd) & ~wmask) | (ror & wmask)) : (ror & wmask);
        u64 top;
        if (opc == 0) { u64 sb = (src >> imms) & 1; top = sb ? ~0ULL : 0ULL; }  /* SBFM */
        else if (opc == 2) top = 0;                                            /* UBFM */
        else top = reg_x(c, Rd);                                              /* BFM */
        u64 r = (top & ~tmask) | (bot & tmask);
        set_x_sz(c, Rd, sf, r);
        return;
    }
    if (t == 0x27) {                              /* EXTR */
        unsigned Rm = BITS(20, 16), imms = BITS(15, 10);
        u64 m = reg_x(c, Rn), n = reg_x(c, Rm);  /* Rn=low(Rm field?), see below */
        /* EXTR Rd, Rn, Rm, #lsb : result = (Rn:Rm) >> lsb */
        u64 hi = reg_x(c, Rn), lo = reg_x(c, Rm);
        (void)m; (void)n;
        unsigned lsb = imms;
        u64 r;
        if (sf) r = lsb ? ((hi << (64 - lsb)) | (lo >> lsb)) : lo;
        else { u32 h = (u32)hi, l = (u32)lo; r = lsb ? (u32)((h << (32 - lsb)) | (l >> lsb)) : l; }
        set_x_sz(c, Rd, sf, r);
        return;
    }
    undefined(c, insn);
}

/* ================= DP-register ================= */
static void dp_register(CPU *c, u32 insn) {
    bool sf = BIT(31);
    unsigned Rd = BITS(4, 0), Rn = BITS(9, 5), Rm = BITS(20, 16);
    unsigned op24 = BITS(28, 24);

    if (op24 == 0x0a) {                            /* logical shifted register */
        unsigned opc = BITS(30, 29), shift = BITS(23, 22), N = BIT(21), imm6 = BITS(15, 10);
        u64 op2 = shift_reg(reg_x(c, Rm), shift, imm6, sf);
        if (N) op2 = ~op2;
        u64 n = reg_x(c, Rn), r;
        switch (opc) {
            case 0: r = n & op2; break;     /* AND/BIC */
            case 1: r = n | op2; break;     /* ORR/ORN */
            case 2: r = n ^ op2; break;     /* EOR/EON */
            default: r = n & op2; break;    /* ANDS/BICS */
        }
        if (!sf) r = (u32)r;
        if (opc == 3) set_logical_flags(c, r, sf);
        set_x(c, Rd, r);
        return;
    }
    if (op24 == 0x0b) {
        bool ext = BIT(21);
        bool op = BIT(30), S = BIT(29);
        u64 op2, n; u32 fl;
        if (ext) {                                 /* add/sub extended register */
            unsigned option = BITS(15, 13), imm3 = BITS(12, 10);
            op2 = extend_reg(reg_x(c, Rm), option, imm3);
            n = reg_xsp(c, Rn);
        } else {                                   /* add/sub shifted register */
            unsigned shift = BITS(23, 22), imm6 = BITS(15, 10);
            op2 = shift_reg(reg_x(c, Rm), shift, imm6, sf);
            n = reg_x(c, Rn);
        }
        u64 r = op ? add_with_carry(n, ~op2, 1, sf, &fl)
                   : add_with_carry(n, op2, 0, sf, &fl);
        if (S) { c->nzcv = fl; set_x_sz(c, Rd, sf, r); }
        else if (ext) set_xsp(c, Rd, sf ? r : (u32)r);
        else set_x_sz(c, Rd, sf, r);
        return;
    }
    if (op24 == 0x1b) {                            /* data processing (3 source) */
        unsigned op31 = BITS(23, 21), o0 = BIT(15), Ra = BITS(14, 10);
        u64 n = reg_x(c, Rn), m = reg_x(c, Rm), a = reg_x(c, Ra), r;
        switch ((op31 << 1) | o0) {
            case 0x0: r = a + n * m; break;                                   /* MADD */
            case 0x1: r = a - n * m; break;                                   /* MSUB */
            case 0x2: r = a + (u64)((s64)(s32)(u32)n * (s64)(s32)(u32)m); break; /* SMADDL */
            case 0x3: r = a - (u64)((s64)(s32)(u32)n * (s64)(s32)(u32)m); break; /* SMSUBL */
            case 0x4: { __int128 p = (__int128)(s64)n * (s64)m; r = (u64)(p >> 64); break; } /* SMULH */
            case 0xa: r = a + (u64)((u64)(u32)n * (u64)(u32)m); break;        /* UMADDL */
            case 0xb: r = a - (u64)((u64)(u32)n * (u64)(u32)m); break;        /* UMSUBL */
            case 0xc: { unsigned __int128 p = (unsigned __int128)n * m; r = (u64)(p >> 64); break; } /* UMULH */
            default: undefined(c, insn); return;
        }
        set_x_sz(c, Rd, sf, r);
        return;
    }
    if (op24 == 0x1a) {
        unsigned op21 = BITS(28, 21);
        if (op21 == 0xd0) {                        /* add/sub with carry */
            bool op = BIT(30), S = BIT(29);
            u32 fl;
            int cin = (c->nzcv & PS_C) ? 1 : 0;
            u64 m = reg_x(c, Rm), n = reg_x(c, Rn);
            u64 r = op ? add_with_carry(n, ~m, cin, sf, &fl)
                       : add_with_carry(n, m, cin, sf, &fl);
            if (S) c->nzcv = fl;
            set_x_sz(c, Rd, sf, r);
            return;
        }
        if (op21 == 0xd2) {                        /* conditional compare */
            bool op = BIT(30);
            unsigned cond = BITS(15, 12), nzcv = BITS(3, 0);
            bool is_imm = BIT(11);
            u64 n = reg_x(c, Rn);
            u64 m = is_imm ? BITS(20, 16) : reg_x(c, Rm);
            if (cond_holds(c, cond)) {
                u32 fl;
                if (op) add_with_carry(n, ~m, 1, sf, &fl);   /* CCMP */
                else add_with_carry(n, m, 0, sf, &fl);       /* CCMN */
                c->nzcv = fl;
            } else {
                c->nzcv = ((nzcv & 8) ? PS_N : 0) | ((nzcv & 4) ? PS_Z : 0) |
                          ((nzcv & 2) ? PS_C : 0) | ((nzcv & 1) ? PS_V : 0);
            }
            return;
        }
        if (op21 == 0xd4) {                        /* conditional select */
            bool op = BIT(30), o2 = BIT(10);
            unsigned cond = BITS(15, 12);
            u64 n = reg_x(c, Rn), m = reg_x(c, Rm), r;
            if (cond_holds(c, cond)) r = n;
            else {
                if (!op && !o2) r = m;             /* CSEL */
                else if (!op && o2) r = m + 1;     /* CSINC */
                else if (op && !o2) r = ~m;        /* CSINV */
                else r = (u64)(-(s64)m);           /* CSNEG */
            }
            set_x_sz(c, Rd, sf, r);
            return;
        }
        if (op21 == 0xd6) {
            if (BIT(30)) {                         /* data processing (1 source) */
                unsigned opcode = BITS(15, 10);
                u64 n = reg_x(c, Rn), r;
                switch (opcode) {
                    case 0x00: {  /* RBIT */
                        u64 v = sf ? n : (u32)n, o = 0;
                        unsigned w = sf ? 64 : 32;
                        for (unsigned i = 0; i < w; i++) if ((v >> i) & 1) o |= 1ULL << (w - 1 - i);
                        r = o; break;
                    }
                    case 0x01: {  /* REV16 */
                        u64 v = n, o = 0; unsigned w = sf ? 8 : 4;
                        for (unsigned i = 0; i < w; i += 2) {
                            o |= ((v >> (i * 8)) & 0xff) << ((i + 1) * 8);
                            o |= ((v >> ((i + 1) * 8)) & 0xff) << (i * 8);
                        }
                        r = o; break;
                    }
                    case 0x02: {  /* REV32 (64-bit) or REV (32-bit) */
                        if (sf) { u64 v = n, o = 0;
                            for (unsigned g = 0; g < 2; g++) {
                                u32 word = (v >> (g * 32)) & 0xffffffff;
                                u32 sw = __builtin_bswap32(word);
                                o |= (u64)sw << (g * 32);
                            }
                            r = o;
                        } else r = __builtin_bswap32((u32)n);
                        break;
                    }
                    case 0x03: r = __builtin_bswap64(n); break;  /* REV (64-bit) */
                    case 0x04: {  /* CLZ */
                        if (sf) r = n ? __builtin_clzll(n) : 64;
                        else r = (u32)n ? __builtin_clz((u32)n) : 32;
                        break;
                    }
                    case 0x05: {  /* CLS */
                        unsigned w = sf ? 64 : 32;
                        u64 v = sf ? n : (u32)n;
                        u64 sign = (v >> (w - 1)) & 1;
                        unsigned cnt = 0;
                        for (int i = w - 2; i >= 0; i--) { if (((v >> i) & 1) == sign) cnt++; else break; }
                        r = cnt; break;
                    }
                    default: undefined(c, insn); return;
                }
                set_x_sz(c, Rd, sf, r);
                return;
            } else {                               /* data processing (2 source) */
                unsigned opcode = BITS(15, 10);
                u64 n = reg_x(c, Rn), m = reg_x(c, Rm), r;
                switch (opcode) {
                    case 0x02: /* UDIV */
                        if (sf) r = (m == 0) ? 0 : n / m;
                        else { u32 a = n, b = m; r = b ? a / b : 0; }
                        break;
                    case 0x03: /* SDIV */
                        if (sf) { s64 a = n, b = m; r = (b == 0) ? 0 : (u64)((b == -1 && a == INT64_MIN) ? a : a / b); }
                        else { s32 a = (s32)(u32)n, b = (s32)(u32)m; r = b ? (u32)((b == -1 && a == INT32_MIN) ? a : a / b) : 0; }
                        break;
                    case 0x08: r = shift_reg(n, 0, m, sf); break;  /* LSLV */
                    case 0x09: r = shift_reg(n, 1, m, sf); break;  /* LSRV */
                    case 0x0a: r = shift_reg(n, 2, m, sf); break;  /* ASRV */
                    case 0x0b: r = shift_reg(n, 3, m, sf); break;  /* RORV */
                    case 0x10: r = crc32_step((u32)n, m, 1, 0xEDB88320); break; /* CRC32B  */
                    case 0x11: r = crc32_step((u32)n, m, 2, 0xEDB88320); break; /* CRC32H  */
                    case 0x12: r = crc32_step((u32)n, m, 4, 0xEDB88320); break; /* CRC32W  */
                    case 0x13: r = crc32_step((u32)n, m, 8, 0xEDB88320); break; /* CRC32X  */
                    case 0x14: r = crc32_step((u32)n, m, 1, 0x82F63B78); break; /* CRC32CB */
                    case 0x15: r = crc32_step((u32)n, m, 2, 0x82F63B78); break; /* CRC32CH */
                    case 0x16: r = crc32_step((u32)n, m, 4, 0x82F63B78); break; /* CRC32CW */
                    case 0x17: r = crc32_step((u32)n, m, 8, 0x82F63B78); break; /* CRC32CX */
                    default: undefined(c, insn); return;
                }
                set_x_sz(c, Rd, sf, r);
                return;
            }
        }
    }
    undefined(c, insn);
}

/* ================= loads/stores ================= */

static u64 ldst_extended_offset(CPU *c, u32 insn, unsigned size) {
    unsigned Rm = BITS(20, 16), option = BITS(15, 13);
    bool S = BIT(12);
    unsigned shift = S ? size : 0;
    /* option==011 => LSL/UXTX (64-bit, no real extend) */
    return extend_reg(reg_x(c, Rm), option, shift);
}

static void do_load(CPU *c, unsigned Rt, u64 va, unsigned size, unsigned opc) {
    unsigned bytes = 1u << size;
    if (opc == 2 && size == 3) return;   /* PRFM: no register write */
    u64 raw;
    if (!mem_read(c, va, bytes, &raw)) return;
    bool sign = (opc == 2) || (opc == 3);
    bool ext64 = (opc == 2) ? true : (opc == 3) ? false : (size == 3);
    u64 val = sign ? sign_extend(raw, bytes * 8) : raw;
    if (!ext64) val = (u32)val;
    set_x(c, Rt, val);
}

/* SIMD/FP register memory access of `bytes` (1,2,4,8,16); zero-extends loads. */
static void vreg_load(CPU *c, unsigned Vt, u64 va, unsigned bytes) {
    V128 val; val.d[0] = 0; val.d[1] = 0;
    if (bytes == 16) { if (!mem_read128(c, va, &val)) return; }
    else { u64 t = 0; if (!mem_read(c, va, bytes, &t)) return; val.d[0] = t; }
    c->v[Vt] = val;
}
static void vreg_store(CPU *c, unsigned Vt, u64 va, unsigned bytes) {
    if (bytes == 16) { mem_write128(c, va, &c->v[Vt]); }
    else mem_write(c, va, bytes, c->v[Vt].d[0]);
}

static void ldst_register(CPU *c, u32 insn) {
    unsigned size = BITS(31, 30), opc = BITS(23, 22);
    unsigned Rn = BITS(9, 5), Rt = BITS(4, 0);
    bool V = BIT(26);
    bool is_store, is_load;
    unsigned bytes, scale;
    if (V) {
        is_load = opc & 1;
        bytes = (opc & 2) ? 16 : (1u << size);
        scale = (opc & 2) ? 4 : size;
    } else {
        is_load = (opc != 0);
        bytes = 1u << size;
        scale = size;
    }
    is_store = !is_load;

    /* compute effective address and optional writeback */
    u64 va, base = reg_xsp(c, Rn);
    int wb = 0;          /* 0 none, 1 post, 2 pre */
    if (BIT(24)) {                              /* unsigned immediate offset */
        va = base + ((u64)BITS(21, 10) << scale);
    } else if (BIT(21)) {                       /* register offset */
        if (BITS(11, 10) != 2) { undefined(c, insn); return; }
        va = base + ldst_extended_offset(c, insn, scale);
    } else {
        unsigned mode = BITS(11, 10);
        s64 imm9 = (s64)sign_extend(BITS(20, 12), 9);
        if (mode == 0 || mode == 2) va = base + imm9; /* unscaled / unprivileged STTR/LDTR */
        else if (mode == 1) { va = base; wb = 1; }  /* post */
        else if (mode == 3) { va = base + imm9; wb = 2; } /* pre */
        else { undefined(c, insn); return; }
        if (wb == 1) base = base + imm9;            /* post writeback value */
    }

    if (V) { if (is_store) vreg_store(c, Rt, va, bytes); else vreg_load(c, Rt, va, bytes); }
    else   { if (is_store) mem_write(c, va, bytes, reg_x(c, Rt)); else do_load(c, Rt, va, size, opc); }

    if (wb == 1) set_xsp(c, Rn, base);
    else if (wb == 2) set_xsp(c, Rn, va);
}

static void ldst_pair(CPU *c, u32 insn) {
    unsigned opc = BITS(31, 30);
    bool V = BIT(26);
    unsigned mode = BITS(25, 23);   /* 000 STNP/LDNP,001 post,010 offset,011 pre */
    bool L = BIT(22);
    s64 imm7 = (s64)sign_extend(BITS(21, 15), 7);
    unsigned Rt2 = BITS(14, 10), Rn = BITS(9, 5), Rt = BITS(4, 0);

    unsigned scale, esz;
    bool signed_word = false;
    if (V) { scale = opc + 2; esz = 1u << scale; }       /* S/D/Q = 4/8/16 bytes */
    else { scale = (opc == 2) ? 3 : 2; esz = 1u << scale; signed_word = (opc == 1); }
    s64 offset = imm7 << scale;

    u64 base = reg_xsp(c, Rn), addr;
    bool wb = false; u64 wbval = 0;
    switch (mode) {
        case 0: addr = base + offset; break;                            /* STNP/LDNP (non-temporal) */
        case 1: addr = base; wb = true; wbval = base + offset; break;   /* post */
        case 2: addr = base + offset; break;                            /* offset */
        case 3: addr = base + offset; wb = true; wbval = addr; break;   /* pre */
        default: undefined(c, insn); return;
    }
    if (V) {
        if (L) { vreg_load(c, Rt, addr, esz); vreg_load(c, Rt2, addr + esz, esz); }
        else { vreg_store(c, Rt, addr, esz); vreg_store(c, Rt2, addr + esz, esz); }
    } else if (L) {
        u64 a, b;
        if (!mem_read(c, addr, esz, &a)) return;
        if (!mem_read(c, addr + esz, esz, &b)) return;
        if (signed_word) { set_x(c, Rt, sign_extend(a, 32)); set_x(c, Rt2, sign_extend(b, 32)); }
        else if (esz == 4) { set_x(c, Rt, (u32)a); set_x(c, Rt2, (u32)b); }
        else { set_x(c, Rt, a); set_x(c, Rt2, b); }
    } else {
        if (!mem_write(c, addr, esz, reg_x(c, Rt))) return;
        if (!mem_write(c, addr + esz, esz, reg_x(c, Rt2))) return;
    }
    if (wb) set_xsp(c, Rn, wbval);
}

static void ldst_literal(CPU *c, u32 insn) {
    unsigned opc = BITS(31, 30);
    bool V = BIT(26);
    if (V) { undefined(c, insn); return; }
    unsigned Rt = BITS(4, 0);
    s64 off = (s64)sign_extend(BITS(23, 5), 19) << 2;
    u64 va = c->cur_insn_pc + off;
    u64 raw;
    switch (opc) {
        case 0: if (mem_read(c, va, 4, &raw)) set_x(c, Rt, (u32)raw); break;       /* LDR Wt */
        case 1: if (mem_read(c, va, 8, &raw)) set_x(c, Rt, raw); break;            /* LDR Xt */
        case 2: if (mem_read(c, va, 4, &raw)) set_x(c, Rt, sign_extend(raw, 32)); break; /* LDRSW */
        default: break;  /* PRFM literal: nop */
    }
}

static void ldst_exclusive(CPU *c, u32 insn) {
    unsigned size = BITS(31, 30);
    bool o2 = BIT(23), L = BIT(22), o1 = BIT(21);
    unsigned Rs = BITS(20, 16), Rt2 = BITS(14, 10), Rn = BITS(9, 5), Rt = BITS(4, 0);
    unsigned bytes = 1u << size;
    u64 va = reg_xsp(c, Rn);

    if (o2) {                                  /* LDAR / STLR (ordered, non-exclusive) */
        if (L) { u64 v; if (mem_read(c, va, bytes, &v)) set_x(c, Rt, v); }
        else mem_write(c, va, bytes, reg_x(c, Rt));
        return;
    }
    if (!o1) {                                 /* single-register exclusive */
        if (L) {                               /* LDXR / LDAXR */
            u64 v;
            if (!mem_read(c, va, bytes, &v)) return;
            set_x(c, Rt, v);
            c->excl_valid = true; c->excl_addr = va; c->excl_size = bytes;
        } else {                               /* STXR / STLXR */
            if (c->excl_valid && c->excl_addr == va) {
                if (!mem_write(c, va, bytes, reg_x(c, Rt))) return;
                set_x(c, Rs, 0);
            } else {
                set_x(c, Rs, 1);
            }
            c->excl_valid = false;
        }
        return;
    }
    /* pair exclusive LDXP/STXP */
    if (L) {
        u64 a, b;
        if (!mem_read(c, va, bytes, &a)) return;
        if (!mem_read(c, va + bytes, bytes, &b)) return;
        set_x(c, Rt, a); set_x(c, Rt2, b);
        c->excl_valid = true; c->excl_addr = va; c->excl_size = bytes * 2;
    } else {
        if (c->excl_valid && c->excl_addr == va) {
            if (!mem_write(c, va, bytes, reg_x(c, Rt))) return;
            if (!mem_write(c, va + bytes, bytes, reg_x(c, Rt2))) return;
            set_x(c, Rs, 0);
        } else set_x(c, Rs, 1);
        c->excl_valid = false;
    }
}

/* AdvSIMD load/store multiple structures: LD1/ST1 (contiguous, 1-4 registers)
 * and LD2/3/4/ST2/3/4 (de-interleaved). Used pervasively for memcpy/memset and
 * NEON block I/O by both EDK2 and Linux. Supports the post-indexed form. */
static void ldst_vector_multi(CPU *c, u32 insn) {
    unsigned Q = BIT(30), post = BIT(23), L = BIT(22), Rm = BITS(20, 16);
    unsigned opcode = BITS(15, 12), size = BITS(11, 10), Rn = BITS(9, 5), Rt = BITS(4, 0);
    unsigned nregs, sel;     /* sel = interleave factor (1 = contiguous LD1/ST1) */
    switch (opcode) {
        case 0x0: nregs = 4; sel = 4; break;   /* LD4/ST4 */
        case 0x2: nregs = 4; sel = 1; break;   /* LD1/ST1 x4 */
        case 0x4: nregs = 3; sel = 3; break;   /* LD3/ST3 */
        case 0x6: nregs = 3; sel = 1; break;   /* LD1/ST1 x3 */
        case 0x7: nregs = 1; sel = 1; break;   /* LD1/ST1 x1 */
        case 0x8: nregs = 2; sel = 2; break;   /* LD2/ST2 */
        case 0xa: nregs = 2; sel = 1; break;   /* LD1/ST1 x2 */
        default: undefined(c, insn); return;
    }
    unsigned regbytes = Q ? 16 : 8;
    u64 base = reg_xsp(c, Rn), addr = base;
    unsigned total = nregs * regbytes;

    if (sel == 1) {                            /* contiguous: whole registers */
        for (unsigned r = 0; r < nregs; r++) {
            unsigned vt = (Rt + r) & 31;
            if (L) {
                V128 v; v.d[0] = v.d[1] = 0;
                if (Q) { if (!mem_read128(c, addr, &v)) return; }
                else   { u64 t; if (!mem_read(c, addr, 8, &t)) return; v.d[0] = t; }
                c->v[vt] = v;
            } else {
                if (Q) { if (!mem_write128(c, addr, &c->v[vt])) return; }
                else   { if (!mem_write(c, addr, 8, c->v[vt].d[0])) return; }
            }
            addr += regbytes;
        }
    } else {                                   /* de-interleaved element by element */
        unsigned ebytes = 1u << size, elems = regbytes / ebytes;
        if (L) for (unsigned r = 0; r < nregs; r++) { c->v[(Rt + r) & 31].d[0] = 0; c->v[(Rt + r) & 31].d[1] = 0; }
        for (unsigned e = 0; e < elems; e++)
            for (unsigned r = 0; r < nregs; r++) {
                unsigned vt = (Rt + r) & 31, off = e * ebytes;
                if (L) { u64 t = 0; if (!mem_read(c, addr, ebytes, &t)) return; memcpy(&c->v[vt].b[off], &t, ebytes); }
                else   { u64 t = 0; memcpy(&t, &c->v[vt].b[off], ebytes); if (!mem_write(c, addr, ebytes, t)) return; }
                addr += ebytes;
            }
    }

    if (post) set_xsp(c, Rn, base + ((Rm == 31) ? total : reg_x(c, Rm)));
}

static void loads_stores(CPU *c, u32 insn) {
    unsigned b2927 = BITS(29, 27);
    if (b2927 == 0x1 && BITS(26, 24) == 0) { ldst_exclusive(c, insn); return; }
    if (b2927 == 0x1 && BIT(26) == 1 && BIT(25) == 0 && BIT(24) == 0) {
        ldst_vector_multi(c, insn); return;    /* AdvSIMD load/store multiple structures */
    }
    if (b2927 == 0x3 && BITS(25, 24) == 0) { ldst_literal(c, insn); return; }
    if (b2927 == 0x5) { ldst_pair(c, insn); return; }
    if (b2927 == 0x7) { ldst_register(c, insn); return; }
    undefined(c, insn);
}

/* ================= branches / exceptions / system ================= */
static void branch_system(CPU *c, u32 insn) {
    unsigned top6 = BITS(31, 26);

    if (top6 == 0x05) { c->pc = c->cur_insn_pc + ((s64)sign_extend(BITS(25, 0), 26) << 2); return; } /* B */
    if (top6 == 0x25) {                                                  /* BL */
        set_x(c, 30, c->cur_insn_pc + 4);
        c->pc = c->cur_insn_pc + ((s64)sign_extend(BITS(25, 0), 26) << 2);
        return;
    }
    if (BITS(31, 24) == 0x54 && BIT(4) == 0) {                            /* B.cond */
        if (cond_holds(c, BITS(3, 0)))
            c->pc = c->cur_insn_pc + ((s64)sign_extend(BITS(23, 5), 19) << 2);
        return;
    }
    if (BITS(30, 25) == 0x1a) {                                           /* CBZ/CBNZ */
        bool sf = BIT(31), op = BIT(24);
        unsigned Rt = BITS(4, 0);
        u64 v = reg_x_sz(c, Rt, sf);
        bool take = op ? (v != 0) : (v == 0);
        if (take) c->pc = c->cur_insn_pc + ((s64)sign_extend(BITS(23, 5), 19) << 2);
        return;
    }
    if (BITS(30, 25) == 0x1b) {                                           /* TBZ/TBNZ */
        bool op = BIT(24);
        unsigned bitpos = (BIT(31) << 5) | BITS(23, 19);
        unsigned Rt = BITS(4, 0);
        u64 v = reg_x(c, Rt);
        bool set = (v >> bitpos) & 1;
        bool take = op ? set : !set;
        if (take) c->pc = c->cur_insn_pc + ((s64)sign_extend(BITS(18, 5), 14) << 2);
        return;
    }
    if (BITS(31, 24) == 0xd4) {                                           /* exception generation */
        unsigned opc = BITS(23, 21), ll = BITS(1, 0), imm16 = BITS(20, 5);
        if (opc == 0 && ll == 1) {                                        /* SVC */
            exception_take(c, EXC_SYNC, esr_make(EC_SVC64, imm16), 0, c->pc);
        } else if (opc == 0 && ll == 2) {                                 /* HVC */
            if (smccc_conduit && smccc_conduit(c, true)) return;
            undefined(c, insn);
        } else if (opc == 0 && ll == 3) {                                 /* SMC */
            if (smccc_conduit && smccc_conduit(c, false)) return;
            undefined(c, insn);
        } else if (opc == 1 && ll == 0) {                                 /* BRK */
            cpu_raise_sync(c, esr_make(EC_BRK64, imm16), 0);
        } else if (opc == 2 && ll == 0) {                                 /* HLT: stop machine (test exit) */
            fprintf(stderr, "[HLT #%u] x0=0x%llx icount=%llu\n", imm16,
                    (unsigned long long)c->x[0], (unsigned long long)c->icount);
            c->stop = true;
        } else undefined(c, insn);
        return;
    }
    if (BITS(31, 25) == 0x6b) {                                           /* unconditional branch (reg) */
        unsigned opc = BITS(24, 21), Rn = BITS(9, 5);
        u64 tgt = reg_x(c, Rn);
        switch (opc) {
            case 0: c->pc = tgt; return;                                  /* BR */
            case 1: set_x(c, 30, c->cur_insn_pc + 4); c->pc = tgt; return;/* BLR */
            case 2: c->pc = reg_x(c, Rn); return;                         /* RET */
            case 4: {                                                     /* ERET */
                u32 spsr = (u32)c->spsr[c->el];
                u64 elr = c->elr[c->el];
                cpu_unpack_spsr(c, spsr);
                c->pc = elr;
                return;
            }
            default: undefined(c, insn); return;
        }
    }
    if (BITS(31, 24) == 0xd5) {                                           /* system instructions */
        unsigned L = BIT(21), op0 = BITS(20, 19), op1 = BITS(18, 16);
        unsigned CRn = BITS(15, 12), CRm = BITS(11, 8), op2 = BITS(7, 5), Rt = BITS(4, 0);
        if (L == 0 && op0 == 0 && op1 == 3 && CRn == 2 && Rt == 31) {     /* hints */
            if (CRm == 0 && op2 == 2) c->halted = true;       /* WFE */
            else if (CRm == 0 && op2 == 3) c->halted = true;  /* WFI */
            /* NOP/YIELD/SEV/SEVL/etc -> no-op */
            return;
        }
        if (L == 0 && op0 == 0 && op1 == 3 && CRn == 3 && Rt == 31) {     /* barriers/CLREX */
            if (op2 == 2) c->excl_valid = false;              /* CLREX */
            /* DSB/DMB/ISB/SB: no-op in an in-order interpreter */
            return;
        }
        /* MSR(imm)/SYS/SYSL/MRS/MSR(reg) -> sysreg.c (M2) */
        if (sysreg_exec) { sysreg_exec(c, insn); return; }
        undefined(c, insn);
        return;
    }
    undefined(c, insn);
}

/* ================= top-level dispatch ================= */
void exec_a64(CPU *c, u32 insn) {
    switch ((insn >> 25) & 0xf) {
        case 0x8: case 0x9: dp_immediate(c, insn); break;     /* 100x */
        case 0xa: case 0xb: branch_system(c, insn); break;    /* 101x */
        case 0x4: case 0x6: case 0xc: case 0xe: loads_stores(c, insn); break; /* x1x0 */
        case 0x5: case 0xd: dp_register(c, insn); break;      /* x101 */
        case 0x7: case 0xf:                                   /* x111 FP/SIMD */
            if (exec_fpsimd) { exec_fpsimd(c, insn); break; }
            if (g_dbg) fprintf(stderr, "[fpsimd] unimpl 0x%08x at pc=0x%llx\n",
                               insn, (unsigned long long)c->cur_insn_pc);
            undefined(c, insn);
            break;
        default: undefined(c, insn); break;                   /* 00xx reserved */
    }
}
