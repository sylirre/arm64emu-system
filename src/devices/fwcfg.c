/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* QEMU fw_cfg device (MMIO), with both the data-port and DMA interfaces.
 * EDK2 reads the kernel/initrd/cmdline through this. */
#include "../devices.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* legacy selector keys */
#define FW_SIGNATURE   0x0000
#define FW_ID          0x0001
#define FW_NB_CPUS     0x0005
#define FW_KERNEL_SIZE 0x0008
#define FW_INITRD_SIZE 0x000b
#define FW_KERNEL_DATA 0x0011
#define FW_INITRD_DATA 0x0012
#define FW_CMDLINE_SIZE 0x0014
#define FW_CMDLINE_DATA 0x0015
#define FW_SETUP_SIZE  0x0017
#define FW_FILE_DIR    0x0019
#define FW_FILE_FIRST  0x0020

/* DMA control bits */
#define DMA_ERROR  1
#define DMA_READ   2
#define DMA_SKIP   4
#define DMA_SELECT 8
#define DMA_WRITE  16

typedef struct { u16 key; u8 *data; u32 len; char name[56]; bool is_file; } FwItem;

struct FwCfg {
    Machine *m;
    FwItem items[96];
    int n;
    u16 next_file_key;
    u16 sel;
    u32 offset;
    u32 dma_hi;
};

static FwItem *find(FwCfg *f, u16 key) {
    for (int i = 0; i < f->n; i++) if (f->items[i].key == key) return &f->items[i];
    return NULL;
}

static FwItem *set_item(FwCfg *f, u16 key, const void *data, u32 len, const char *name) {
    FwItem *it = find(f, key);
    if (!it) {
        if (f->n >= (int)(sizeof f->items / sizeof f->items[0])) {
            fprintf(stderr, "fw_cfg: item table full (%d entries), dropping key 0x%x\n",
                    f->n, key);
            return NULL;
        }
        it = &f->items[f->n++]; it->key = key;
    }
    else free(it->data);
    it->data = malloc(len ? len : 1);
    if (data) memcpy(it->data, data, len);
    else memset(it->data, 0, len);
    it->len = len;
    it->is_file = name != NULL;
    if (name) { strncpy(it->name, name, 55); it->name[55] = 0; }
    return it;
}

static void put_be32(u8 *p, u32 v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

static void rebuild_dir(FwCfg *f) {
    int nfiles = 0;
    for (int i = 0; i < f->n; i++) if (f->items[i].is_file) nfiles++;
    u32 sz = 4 + (u32)nfiles * 64;
    u8 *buf = calloc(1, sz);
    put_be32(buf, (u32)nfiles);
    u8 *p = buf + 4;
    for (int i = 0; i < f->n; i++) {
        FwItem *it = &f->items[i];
        if (!it->is_file) continue;
        put_be32(p + 0, it->len);
        p[4] = it->key >> 8; p[5] = it->key & 0xff;  /* select (BE16) */
        p[6] = 0; p[7] = 0;
        strncpy((char *)(p + 8), it->name, 55);
        p += 64;
    }
    set_item(f, FW_FILE_DIR, buf, sz, NULL);
    free(buf);
}

void fwcfg_add_file(FwCfg *f, const char *name, const void *data, u32 len) {
    set_item(f, f->next_file_key++, data, len, name);
    rebuild_dir(f);
}

void fwcfg_set_legacy_kernel(FwCfg *f, const void *kernel, u32 ksize,
                             const void *initrd, u32 isize, const char *cmdline) {
    u8 le[4];
    le[0]=ksize; le[1]=ksize>>8; le[2]=ksize>>16; le[3]=ksize>>24;
    set_item(f, FW_KERNEL_SIZE, le, 4, NULL);
    set_item(f, FW_KERNEL_DATA, kernel, ksize, NULL);
    le[0]=isize; le[1]=isize>>8; le[2]=isize>>16; le[3]=isize>>24;
    set_item(f, FW_INITRD_SIZE, le, 4, NULL);
    set_item(f, FW_INITRD_DATA, initrd, isize, NULL);
    u32 clen = cmdline ? (u32)strlen(cmdline) + 1 : 0;
    le[0]=clen; le[1]=clen>>8; le[2]=clen>>16; le[3]=clen>>24;
    set_item(f, FW_CMDLINE_SIZE, le, 4, NULL);
    set_item(f, FW_CMDLINE_DATA, cmdline, clen, NULL);
    u8 zero[4] = {0};
    set_item(f, FW_SETUP_SIZE, zero, 4, NULL);
}

/* ---- MMIO ---- */
static void do_dma(FwCfg *f, u64 addr) {
    Machine *m = f->m;
    u8 hdr[16];
    phys_read_blk(m, addr, hdr, 16);
    u32 control = (hdr[0]<<24)|(hdr[1]<<16)|(hdr[2]<<8)|hdr[3];
    u32 length  = (hdr[4]<<24)|(hdr[5]<<16)|(hdr[6]<<8)|hdr[7];
    u64 target  = ((u64)hdr[8]<<56)|((u64)hdr[9]<<48)|((u64)hdr[10]<<40)|((u64)hdr[11]<<32)|
                  ((u64)hdr[12]<<24)|((u64)hdr[13]<<16)|((u64)hdr[14]<<8)|hdr[15];

    if (control & DMA_SELECT) { f->sel = control >> 16; f->offset = 0; }

    if (control & DMA_READ) {
        FwItem *it = find(f, f->sel);
        if (g_dbg >= 2)
            fprintf(stderr, "[fwcfg] DMA read sel=0x%x off=%u len=%u present=%d itemlen=%u icount=%llu\n",
                    f->sel, f->offset, length, it != NULL, it ? it->len : 0,
                    (unsigned long long)m->cpu.icount);
        for (u32 i = 0; i < length; i++) {
            u8 b = (it && f->offset < it->len) ? it->data[f->offset] : 0;
            phys_write(m, target + i, 1, b);
            f->offset++;
        }
    } else if (control & (DMA_WRITE | DMA_SKIP)) {
        f->offset += length;
    }
    /* signal completion: control = 0 (clears error/ownership) */
    u8 zero[4] = {0};
    phys_write_blk(m, addr, zero, 4);
}

static u64 fwcfg_read(void *opaque, u64 off, unsigned size) {
    FwCfg *f = opaque;
    if (off == 0x00) {                          /* data register: byte stream */
        FwItem *it = find(f, f->sel);
        u64 v = 0;
        for (unsigned i = 0; i < size; i++) {
            u8 b = (it && f->offset < it->len) ? it->data[f->offset] : 0;
            v |= (u64)b << (i * 8);
            f->offset++;
        }
        return v;
    }
    if (off == 0x10) return 0;                   /* DMA addr readback (unused) */
    return 0;
}

static void fwcfg_write(void *opaque, u64 off, unsigned size, u64 val) {
    FwCfg *f = opaque;
    if (off == 0x08) {                           /* selector (big-endian 16-bit) */
        f->sel = (u16)((val >> 8) | (val << 8));
        f->offset = 0;
        return;
    }
    if (off == 0x10) {
        if (size == 8) { do_dma(f, __builtin_bswap64(val)); return; }
        f->dma_hi = __builtin_bswap32((u32)val); return;   /* high half */
    }
    if (off == 0x14) {                           /* low half -> trigger */
        u32 lo = __builtin_bswap32((u32)val);
        do_dma(f, ((u64)f->dma_hi << 32) | lo);
        return;
    }
}

/* Return fw_cfg to power-on selector state (system reset). The item table
 * (signature, kernel/initrd/cmdline payloads, file dir) is static for the run,
 * so it persists; only the in-flight selector/offset/DMA cursor is cleared. */
void fwcfg_reset(FwCfg *f) {
    f->sel = 0;
    f->offset = 0;
    f->dma_hi = 0;
}

FwCfg *fwcfg_create(Machine *m) {
    FwCfg *f = calloc(1, sizeof(*f));
    f->m = m;
    f->next_file_key = FW_FILE_FIRST;
    set_item(f, FW_SIGNATURE, "QEMU", 4, NULL);
    u8 id[4] = { 3, 0, 0, 0 };                   /* traditional + DMA */
    set_item(f, FW_ID, id, 4, NULL);
    u8 ncpu[2] = { 1, 0 };
    set_item(f, FW_NB_CPUS, ncpu, 2, NULL);
    rebuild_dir(f);
    machine_add_device(m, FWCFG_BASE, 0x18, fwcfg_read, fwcfg_write, f, "fw-cfg");
    m->fwcfg = f;
    return f;
}
