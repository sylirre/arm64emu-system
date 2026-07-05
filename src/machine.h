/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Machine: physical memory bus (RAM + flash + MMIO devices). */
#ifndef A64_MACHINE_H
#define A64_MACHINE_H

#include "types.h"
#include "cpu.h"

/* Default platform constants (QEMU 'virt', GICv2). */
#define FLASH_BASE   0x00000000ULL
#define FLASH_SIZE   0x08000000ULL   /* 128 MB: 64 MB code + 64 MB vars */
#define FLASH_BANK   0x04000000ULL   /* 64 MB per bank */
#define RAM_BASE     0x40000000ULL
#define RAM_SIZE_DEF (1024ULL << 20) /* 1 GB default */

/* Memory abort reporting from the bus (unmapped access). */
typedef enum { BUS_OK = 0, BUS_FAULT = 1 } BusStatus;

typedef u64  (*mmio_read_fn)(void *opaque, u64 offset, unsigned size);
typedef void (*mmio_write_fn)(void *opaque, u64 offset, unsigned size, u64 value);

typedef struct {
    u64 base, size;
    mmio_read_fn read;
    mmio_write_fn write;
    void *opaque;
    const char *name;
} MMIODev;

#define MAX_DEVS 48
/* Max attachable disks. Ceiling is the 31 non-net virtio-mmio slots (slot 0 is
 * reserved for virtio-net); 8 is plenty and keeps the Machine arrays small. */
#define MAX_DRIVES 8
/* Max host directory shares (virtio-9p). Same 31-slot ceiling shared with disks
 * (net=slot 0, then disks, then shares); 8 keeps the Machine arrays small. */
#define MAX_SHARES 8

struct GIC;
struct PL011;
struct PL031;
struct FwCfg;
struct ARMTimer;
struct VirtIOBlk;
struct VirtIONet;
struct VirtIO9P;
struct VirtIOConsole;

typedef struct NetFwd {
    bool is_udp;
    int  host_port;
    int  guest_port;
} NetFwd;

/* A -virtfs host directory share: exported to the guest as a virtio-9p device
 * with mount tag `tag`; `ro` maps the whole tree read-only. */
typedef struct VirtFS {
    const char *path;   /* host directory to export */
    const char *tag;    /* 9p mount tag (guest `mount -t 9p <tag> ...`) */
    bool        ro;     /* read-only share */
} VirtFS;

/* A -drive disk: backing image `path`; `ro` opens it O_RDONLY and advertises
 * VIRTIO_BLK_F_RO so the guest mounts it read-only and writes are rejected. */
typedef struct Drive {
    const char *path;   /* host image file */
    bool        ro;     /* read-only disk */
} Drive;

typedef struct Machine {
    CPU cpu;

    u8 *ram;  u64 ram_base, ram_size;
    u8 *flash; u64 flash_base, flash_size;
    bool flash_writable;          /* simple RAM-backed flash writes (CFI added later) */

    /* NOR flash CFI (Intel pflash_cfi01) state, per 64 MB bank. EDK2 keeps its
     * UEFI variable store in the flash and drives it with the Intel command set
     * (read-id/CFI, block erase, word/buffer program, status polling). */
    struct FlashCFI {
        u8  mode;       /* read mode: array/status/id/cfi */
        u8  prog;       /* program/erase phase state machine */
        u8  status;     /* status register (0x80 = ready) */
        u32 buf_base;   /* buffered-program base offset */
        u32 buf_cnt;    /* buffered-program word count */
        u32 buf_seen;   /* buffered words written so far */
    } flash_cfi[2];

    MMIODev dev[MAX_DEVS];
    int ndev;

    /* Device instances (owned). */
    struct GIC      *gic;
    struct ARMTimer *timer;
    struct PL011    *uart;
    struct PL031    *rtc;
    struct FwCfg    *fwcfg;
    struct VirtIOBlk *blk[MAX_DRIVES];  /* attached disks, in attach order */
    int    n_blk;
    struct VirtIONet *net;
    struct VirtIO9P  *fs[MAX_SHARES];   /* attached 9p shares, in attach order */
    int    n_fs;
    struct VirtIOConsole *vcon;         /* -console virtio (hvc0), else NULL */

    Drive  drives[MAX_DRIVES];   /* -drive disks */
    int    n_drives;
    VirtFS shares[MAX_SHARES];    /* -virtfs host directory shares */
    int    n_shares;
    bool net_enabled;             /* -net flag */
    bool console_virtio;          /* -console virtio (default false = pl011) */
    bool console_active_virtio;   /* input follows output: true once hvc0 (not
                                   * ttyAMA0) last produced output; routes host
                                   * stdin to the console the guest is actually
                                   * using. Only consulted when console_virtio. */
    NetFwd net_fwds[16];          /* -netfwd rules */
    int    n_net_fwds;

    BusStatus last_bus_status;    /* set by phys_* on fault */
} Machine;

void machine_init(Machine *m, u64 ram_size);
void machine_free(Machine *m);

/* Return the NOR flash CFI command state machine to read-array/ready (reset). */
void flash_cfi_reset(Machine *m);
void machine_add_device(Machine *m, u64 base, u64 size, mmio_read_fn r,
                        mmio_write_fn w, void *opaque, const char *name);

/* Physical memory access. On unmapped access, sets m->last_bus_status=BUS_FAULT
 * and returns 0 (reads) / ignores (writes). `size` is 1,2,4,8. */
u64  phys_read(Machine *m, u64 pa, unsigned size);
void phys_write(Machine *m, u64 pa, unsigned size, u64 value);

/* Bulk copy helpers (used by loaders / fw_cfg DMA). Truncated to mapped RAM. */
void phys_write_blk(Machine *m, u64 pa, const void *src, u64 len);
void phys_read_blk(Machine *m, u64 pa, void *dst, u64 len);

/* Direct host pointer into RAM for [pa, pa+len), or NULL if out of range. */
void *ram_ptr(Machine *m, u64 pa, u64 len);

#endif /* A64_MACHINE_H */
