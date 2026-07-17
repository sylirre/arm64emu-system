/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Physical memory bus: RAM, flash, and MMIO device dispatch. */
#include "machine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>

void machine_init(Machine *m, u64 ram_size) {
    memset(m, 0, sizeof(*m));
    m->ram_base = RAM_BASE;
    m->ram_size = ram_size ? ram_size : RAM_SIZE_DEF;
    /* mmap, not calloc: host page pointers to RAM must be 4 KB-aligned because
     * the D-TLB packs permission bits into their low bits (see mmu.h), and the
     * anonymous mapping gives lazily-zeroed pages for large guest RAM. */
    m->ram = mmap(NULL, m->ram_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m->ram == MAP_FAILED) m->ram = NULL;
    m->flash_base = FLASH_BASE;
    m->flash_size = FLASH_SIZE;
    m->flash = calloc(1, m->flash_size);
    m->flash_writable = true;
    m->flash_cfi[0].status = m->flash_cfi[1].status = 0x80;  /* CFI status: WSM ready */
    if (!m->ram || !m->flash) {
        fprintf(stderr, "fatal: out of memory allocating guest RAM/flash\n");
        exit(1);
    }
    m->ndev = 0;
    m->cpu.m = m;
}

void machine_free(Machine *m) {
    if (m->ram) munmap(m->ram, m->ram_size);
    free(m->flash);
}

void machine_add_device(Machine *m, u64 base, u64 size, mmio_read_fn r,
                        mmio_write_fn w, void *opaque, const char *name) {
    if (m->ndev >= MAX_DEVS) {
        fprintf(stderr, "fatal: too many MMIO devices\n");
        exit(1);
    }
    MMIODev *d = &m->dev[m->ndev++];
    d->base = base; d->size = size; d->read = r; d->write = w;
    d->opaque = opaque; d->name = name;
}

/* ---------- NOR flash: Intel CFI (pflash_cfi01) command set ----------
 * EDK2 ArmVirtQemu stores UEFI variables in flash and drives it with the Intel
 * command set. We complete every program/erase instantly (status always ready).
 * Instruction fetch reads the array directly (mem_ifetch fast path) and is
 * unaffected by the command mode, matching real hardware (code runs from a bank
 * left in read-array mode). Block size matches EDK2's NorFlashQemuLib: 256 KB. */
enum { FL_ARRAY = 0, FL_STATUS, FL_ID, FL_CFI };
enum { PG_NONE = 0, PG_WORD, PG_ERASE, PG_BUFCNT, PG_BUFDATA, PG_BUFCONF };
#define FL_BLOCK   0x40000ULL          /* 256 KB erase block (SIZE_256KB) */
#define FL_STAT_RDY 0x80               /* SR.7: write state machine ready */

/* Program (clear bits) `size` bytes at flash offset `off` with `val`. */
static void flash_program(Machine *m, u64 off, unsigned size, u64 val) {
    for (unsigned i = 0; i < size; i++)
        m->flash[off + i] &= (u8)(val >> (i * 8));
}

/* CFI command write to the flash bank containing `off`. */
static void flash_cfi_write(Machine *m, u64 off, unsigned size, u64 val) {
    struct FlashCFI *s = &m->flash_cfi[off >= FLASH_BANK ? 1 : 0];
    unsigned cmd = val & 0xff;

    switch (s->prog) {                 /* data phases consume the write as data */
    case PG_WORD:
        flash_program(m, off, size, val); s->prog = PG_NONE; s->mode = FL_STATUS; return;
    case PG_BUFCNT:
        s->buf_cnt = (u32)((val & 0xffff) + 1); s->buf_seen = 0;
        s->buf_base = (u32)off; s->prog = PG_BUFDATA; return;
    case PG_BUFDATA:
        flash_program(m, off, size, val);
        if (++s->buf_seen >= s->buf_cnt) s->prog = PG_BUFCONF;
        return;
    case PG_BUFCONF:
        if (cmd == 0xd0) { s->prog = PG_NONE; s->mode = FL_STATUS; }
        return;
    case PG_ERASE:
        if (cmd == 0xd0) {             /* erase confirm: wipe the 256 KB block */
            u64 base = off & ~(FL_BLOCK - 1);
            memset(m->flash + base, 0xff, FL_BLOCK);
            s->status = FL_STAT_RDY; s->prog = PG_NONE; s->mode = FL_STATUS;
        }
        return;
    }

    switch (cmd) {                     /* command phase */
    case 0xff: case 0xf0: s->mode = FL_ARRAY;  break;   /* read array */
    case 0x70:            s->mode = FL_STATUS; break;   /* read status */
    case 0x50:            s->status = FL_STAT_RDY; break; /* clear status */
    case 0x90:            s->mode = FL_ID;     break;   /* read identifier */
    case 0x98:            s->mode = FL_CFI;    break;   /* CFI query */
    case 0x10: case 0x40: s->prog = PG_WORD;   s->mode = FL_STATUS; break; /* word program */
    case 0x20:            s->prog = PG_ERASE;  s->mode = FL_STATUS; break; /* block erase */
    case 0xe8:            s->prog = PG_BUFCNT; s->mode = FL_STATUS; break; /* buffer program */
    default: break;
    }
}

/* Minimal Intel CFI query table (byte at CFI word offset). */
static u8 cfi_query_byte(u64 word_off) {
    switch (word_off) {
    case 0x10: return 'Q';  case 0x11: return 'R';  case 0x12: return 'Y';
    case 0x13: return 0x01; case 0x14: return 0x00;       /* primary cmd set: Intel */
    case 0x27: return 0x1a;                                /* device size: 2^26 = 64 MB */
    case 0x28: return 0x02; case 0x29: return 0x00;       /* x16 async interface */
    case 0x2a: return 0x05; case 0x2b: return 0x00;       /* max write buffer 2^5 = 32 B */
    case 0x2c: return 0x01;                                /* one erase region */
    case 0x2d: return 0xff; case 0x2e: return 0x00;       /* (256-1) blocks */
    case 0x2f: return 0x00; case 0x30: return 0x04;       /* 0x0400*256 = 256 KB block */
    default:   return 0x00;
    }
}

/* CFI read from the flash bank containing `off`. Returns true if handled (mode
 * other than read-array); *out holds the value. */
static bool flash_cfi_read(Machine *m, u64 off, unsigned size, u64 *out) {
    struct FlashCFI *s = &m->flash_cfi[off >= FLASH_BANK ? 1 : 0];
    if (s->mode == FL_ARRAY) return false;          /* normal array read */
    u64 bank_off = off & (FLASH_BANK - 1);
    u64 v = 0;
    for (unsigned i = 0; i < size; i++) {
        u8 b = 0;
        if (s->mode == FL_STATUS) b = (i & 1) ? 0 : s->status;     /* SR in each x16 lane */
        else if (s->mode == FL_ID) {
            u64 wo = (bank_off + i) >> 1;            /* x16 word offset */
            b = ((bank_off + i) & 1) ? 0 : (wo == 0 ? 0x89 : wo == 1 ? 0x18 : 0x00);
        } else /* FL_CFI */ {
            u64 wo = (bank_off + i) >> 1;
            b = ((bank_off + i) & 1) ? 0 : cfi_query_byte(wo);
        }
        v |= (u64)b << (i * 8);
    }
    *out = v;
    return true;
}

/* Return the NOR flash command state machine to read-array/ready on system
 * reset. The array contents (UEFI variable store) are non-volatile and left
 * intact; this only clears a program/erase left in flight when the reset hit. */
void flash_cfi_reset(Machine *m) {
    for (int b = 0; b < 2; b++) {
        m->flash_cfi[b].mode   = FL_ARRAY;
        m->flash_cfi[b].prog   = PG_NONE;
        m->flash_cfi[b].status = FL_STAT_RDY;
        m->flash_cfi[b].buf_base = m->flash_cfi[b].buf_cnt = m->flash_cfi[b].buf_seen = 0;
    }
}

static MMIODev *find_dev(Machine *m, u64 pa) {
    for (int i = 0; i < m->ndev; i++) {
        MMIODev *d = &m->dev[i];
        if (pa >= d->base && pa < d->base + d->size) return d;
    }
    return NULL;
}

void *ram_ptr(Machine *m, u64 pa, u64 len) {
    if (pa >= m->ram_base && pa + len <= m->ram_base + m->ram_size)
        return m->ram + (pa - m->ram_base);
    return NULL;
}

/* Host is little-endian; guest is little-endian. Direct memcpy preserves order. */
u64 phys_read(Machine *m, u64 pa, unsigned size) {
    m->last_bus_status = BUS_OK;
    if (g_dbg >= 2 && pa >= RAM_BASE && pa < RAM_BASE + 0x40) {
        static int n = 0;
        if (n++ < 40) fprintf(stderr, "[dtbrd] pa=0x%llx size=%u pc=0x%llx\n",
                              (unsigned long long)pa, size, (unsigned long long)m->cpu.cur_insn_pc);
    }
    if (pa >= m->ram_base && pa + size <= m->ram_base + m->ram_size) {
        u64 v = 0;
        memcpy(&v, m->ram + (pa - m->ram_base), size);
        return v;
    }
    if (pa >= m->flash_base && pa + size <= m->flash_base + m->flash_size) {
        u64 off = pa - m->flash_base, v = 0;
        if (flash_cfi_read(m, off, size, &v)) return v;   /* status/id/cfi mode */
        memcpy(&v, m->flash + off, size);                 /* read-array mode */
        return v;
    }
    MMIODev *d = find_dev(m, pa);
    if (d && d->read) return d->read(d->opaque, pa - d->base, size);

    m->last_bus_status = BUS_FAULT;
    if (g_trace)
        fprintf(stderr, "[bus] unmapped read  pa=0x%llx size=%u\n",
                (unsigned long long)pa, size);
    return 0;
}

void phys_write(Machine *m, u64 pa, unsigned size, u64 value) {
    m->last_bus_status = BUS_OK;
    if (g_dbg >= 3 && pa >= 0x40000000 && pa < 0x40000020)
        fprintf(stderr, "[wwatch] pa=0x%llx size=%u val=0x%llx pc=0x%llx\n",
                (unsigned long long)pa, size, (unsigned long long)value,
                (unsigned long long)m->cpu.cur_insn_pc);
    if (g_watch && pa < g_watch + 8 && pa + size > g_watch)
        fprintf(stderr, "[watch] pa=0x%llx size=%u val=0x%llx pc=0x%llx icount=%llu\n",
                (unsigned long long)pa, size, (unsigned long long)value,
                (unsigned long long)m->cpu.cur_insn_pc,
                (unsigned long long)m->cpu.icount);
    if (pa >= m->ram_base && pa + size <= m->ram_base + m->ram_size) {
        memcpy(m->ram + (pa - m->ram_base), &value, size);
        return;
    }
    if (pa >= m->flash_base && pa + size <= m->flash_base + m->flash_size) {
        flash_cfi_write(m, pa - m->flash_base, size, value);   /* Intel CFI commands */
        return;
    }
    MMIODev *d = find_dev(m, pa);
    if (d && d->write) { d->write(d->opaque, pa - d->base, size, value); return; }

    m->last_bus_status = BUS_FAULT;
    if (g_trace)
        fprintf(stderr, "[bus] unmapped write pa=0x%llx size=%u val=0x%llx\n",
                (unsigned long long)pa, size, (unsigned long long)value);
}

void phys_write_blk(Machine *m, u64 pa, const void *src, u64 len) {
    const u8 *s = src;
    /* RAM fast path */
    if (pa >= m->ram_base && pa + len <= m->ram_base + m->ram_size) {
        memcpy(m->ram + (pa - m->ram_base), s, len);
        return;
    }
    if (pa >= m->flash_base && pa + len <= m->flash_base + m->flash_size) {
        memcpy(m->flash + (pa - m->flash_base), s, len);
        return;
    }
    for (u64 i = 0; i < len; i++) phys_write(m, pa + i, 1, s[i]);
}

void phys_read_blk(Machine *m, u64 pa, void *dst, u64 len) {
    u8 *d = dst;
    if (pa >= m->ram_base && pa + len <= m->ram_base + m->ram_size) {
        memcpy(d, m->ram + (pa - m->ram_base), len);
        return;
    }
    if (pa >= m->flash_base && pa + len <= m->flash_base + m->flash_size) {
        memcpy(d, m->flash + (pa - m->flash_base), len);
        return;
    }
    for (u64 i = 0; i < len; i++) d[i] = (u8)phys_read(m, pa + i, 1);
}
