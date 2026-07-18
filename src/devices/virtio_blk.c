/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* virtio-blk over a modern (version 2) virtio-mmio transport, slot 0 of QEMU's
 * 'virt' map (0x0a000000, INTID 48 / SPI 16, edge-triggered per the DTB).
 *
 * The device is backed by a host image file opened O_RDWR; guest writes go
 * through to the file via pwrite. Requests are completed synchronously inside
 * the QueueNotify write and the IRQ is raised inline — which fits this
 * single-threaded deterministic interpreter: the guest isn't running while we
 * service the queue, and the kernel's ISR drains the whole used ring per IRQ. */
#include "../devices.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define SECTOR_SIZE   512
#define QUEUE_NUM_MAX 256        /* QueueNumMax for the single requestq */
#define VIRTQ_MAX     256        /* descriptor-chain walk bound (== QueueNumMax) */

/* Feature bits we advertise: VIRTIO_F_VERSION_1 (bit 32), mandatory for a modern
 * device, plus VIRTIO_BLK_F_RO (bit 5) for read-only disks. VIRTIO_BLK_F_RO is
 * added per-instance in virtio_blk_create; the base set stays minimal. */
#define VIRTIO_F_VERSION_1 (1ULL << 32)
#define VIRTIO_BLK_F_RO    (1ULL << 5)
#define VIRTIO_BLK_F_FLUSH (1ULL << 9)   /* device honors VIRTIO_BLK_T_FLUSH (fsync) */
#define BLK_FEATURES       (VIRTIO_F_VERSION_1 | VIRTIO_BLK_F_FLUSH)

/* Split-virtqueue descriptor flags. */
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

/* virtio-blk request types and status codes. */
#define VIRTIO_BLK_T_IN      0
#define VIRTIO_BLK_T_OUT     1
#define VIRTIO_BLK_T_FLUSH   4
#define VIRTIO_BLK_T_GET_ID  8
#define VIRTIO_BLK_S_OK      0
#define VIRTIO_BLK_S_IOERR   1
#define VIRTIO_BLK_S_UNSUPP  2

typedef struct VirtIOBlk {
    Machine *m; GIC *gic; int irq;     /* INTID_VIRTIO0 + slot */
    int index;                         /* attach order (0-based); used for the serial */
    int fd; u64 capacity;              /* backing image; capacity in 512B sectors */
    bool ro;                           /* read-only disk (advertises VIRTIO_BLK_F_RO) */
    u64 features;                      /* device feature bits (BLK_FEATURES [| RO]) */

    u32 status, isr;                   /* Status, InterruptStatus */
    u32 dev_feat_sel, drv_feat_sel; u64 drv_feat;
    u32 queue_sel;

    /* single queue (index 0) */
    u32 q_num, q_ready;
    u64 q_desc, q_avail, q_used;
    u16 last_avail;                    /* our cursor into the available ring */
} VirtIOBlk;

static void blk_reset(VirtIOBlk *v) {
    v->status = 0; v->isr = 0;
    v->dev_feat_sel = v->drv_feat_sel = 0; v->drv_feat = 0;
    v->queue_sel = 0;
    v->q_num = 0; v->q_ready = 0;
    v->q_desc = v->q_avail = v->q_used = 0;
    v->last_avail = 0;
    gic_set_irq(v->gic, v->irq, 0);
}

/* Append {id, len} to the used ring and publish it (element before the index). */
static void blk_push_used(VirtIOBlk *v, u32 id, u32 len) {
    Machine *m = v->m;
    u16 used_idx = (u16)phys_read(m, v->q_used + 2, 2);
    u64 e = v->q_used + 4 + (u64)(used_idx % v->q_num) * 8;
    phys_write(m, e + 0, 4, id);
    phys_write(m, e + 4, 4, len);
    phys_write(m, v->q_used + 2, 2, (u16)(used_idx + 1));
}

/* Service one request: walk its descriptor chain, do the I/O, write the status
 * byte, and retire it on the used ring. The first descriptor is the 16-byte
 * header (readable), the last is the 1-byte status (writable), the rest is data. */
static void blk_request(VirtIOBlk *v, u32 head) {
    Machine *m = v->m;
    u64 addr[VIRTQ_MAX]; u32 dlen[VIRTQ_MAX]; u16 dflags[VIRTQ_MAX];
    u32 n = 0, idx = head;
    for (;;) {
        if (n >= v->q_num) break;                    /* malformed / loop guard */
        u64 d = v->q_desc + (u64)idx * 16;
        addr[n]   = phys_read(m, d + 0, 8);
        dlen[n]   = (u32)phys_read(m, d + 8, 4);
        dflags[n] = (u16)phys_read(m, d + 12, 2);
        u16 next  = (u16)phys_read(m, d + 14, 2);
        n++;
        if (!(dflags[n - 1] & VIRTQ_DESC_F_NEXT)) break;
        idx = next;
    }
    if (n < 2) return;                               /* need header + status */

    u64 hdr        = addr[0];
    u32 type       = (u32)phys_read(m, hdr + 0, 4);
    u64 sector     = phys_read(m, hdr + 8, 8);
    u64 status_addr = addr[n - 1];
    u8  status     = VIRTIO_BLK_S_OK;
    u32 used_len   = 0;

    u8 buf[4096];
    u64 off = sector * SECTOR_SIZE;

    /* Data descriptors are the middle ones [1 .. n-2]. */
    if (type == VIRTIO_BLK_T_OUT && v->ro) {
        status = VIRTIO_BLK_S_IOERR;                 /* write to read-only disk */
    } else if (type == VIRTIO_BLK_T_IN || type == VIRTIO_BLK_T_OUT) {
        u64 total = 0;
        for (u32 i = 1; i + 1 < n; i++) total += dlen[i];
        /* Range-check without overflow: a hostile `sector` (~2^55) would wrap
         * both `sector*512` and `off+total`. Compare in the sector/byte domains
         * with subtraction so nothing overflows. */
        if (sector > v->capacity || total > (v->capacity - sector) * SECTOR_SIZE) {
            status = VIRTIO_BLK_S_IOERR;             /* out of range */
        } else {
            for (u32 i = 1; i + 1 < n; i++) {
                u64 gpa = addr[i]; u32 rem = dlen[i];
                while (rem) {
                    u32 chunk = rem < sizeof(buf) ? rem : (u32)sizeof(buf);
                    if (type == VIRTIO_BLK_T_IN) {
                        memset(buf, 0, chunk);       /* short read => zero-filled tail */
                        if (pread(v->fd, buf, chunk, (off_t)off) < 0) {
                            status = VIRTIO_BLK_S_IOERR;
                            goto blk_io_done;        /* abort; don't count the failed chunk */
                        }
                        phys_write_blk(m, gpa, buf, chunk);
                        used_len += chunk;           /* data written to guest */
                    } else {
                        phys_read_blk(m, gpa, buf, chunk);
                        if (pwrite(v->fd, buf, chunk, (off_t)off) != (ssize_t)chunk)
                            status = VIRTIO_BLK_S_IOERR;
                    }
                    off += chunk; gpa += chunk; rem -= chunk;
                }
            }
        blk_io_done: ;
        }
    } else if (type == VIRTIO_BLK_T_GET_ID) {
        char id[20]; snprintf(id, sizeof(id), "arm64emu-vblk%d", v->index);
        if (n >= 3) {
            u8 idbuf[20]; memset(idbuf, 0, sizeof(idbuf));
            memcpy(idbuf, id, strlen(id));
            u32 cap = dlen[1] < sizeof(idbuf) ? dlen[1] : (u32)sizeof(idbuf);
            phys_write_blk(m, addr[1], idbuf, cap);
            used_len += cap;
        }
    } else if (type == VIRTIO_BLK_T_FLUSH) {
        fsync(v->fd);
    } else {
        status = VIRTIO_BLK_S_UNSUPP;
    }

    phys_write(m, status_addr, 1, status);
    used_len += 1;                                   /* status byte is device-written */
    blk_push_used(v, head, used_len);
}

/* Drain the available ring, then raise the queue interrupt once. */
static void virtio_blk_process(VirtIOBlk *v) {
    Machine *m = v->m;
    if (!v->q_ready || v->q_num == 0) return;
    u16 avail_idx = (u16)phys_read(m, v->q_avail + 2, 2);
    bool did = false;
    while (v->last_avail != avail_idx) {
        u16 hd = (u16)phys_read(m, v->q_avail + 4 + (u64)(v->last_avail % v->q_num) * 2, 2);
        blk_request(v, hd);
        v->last_avail++;
        did = true;
    }
    if (did) {
        v->isr |= 1;                                 /* VIRTIO_MMIO_INT_VRING */
        gic_set_irq(v->gic, v->irq, 1);              /* edge: rising latches pending */
    }
}

/* Device config space (struct virtio_blk_config): only `capacity` (u64 @ 0) is
 * meaningful; every other field reads as 0 (no optional features advertised). */
static u64 blk_config_read(VirtIOBlk *v, u64 off, unsigned size) {
    u8 cfg[64];
    memset(cfg, 0, sizeof(cfg));
    for (int i = 0; i < 8; i++) cfg[i] = (u8)(v->capacity >> (i * 8));
    u64 r = 0;
    for (unsigned i = 0; i < size && off + i < sizeof(cfg); i++)
        r |= (u64)cfg[off + i] << (i * 8);
    return r;
}

static u64 blk_read(void *opaque, u64 off, unsigned size) {
    VirtIOBlk *v = opaque;
    if (off >= 0x100) return blk_config_read(v, off - 0x100, size);
    switch (off) {
        case 0x000: return 0x74726976;               /* MagicValue "virt" */
        case 0x004: return 2;                        /* Version (modern)  */
        case 0x008: return 2;                        /* DeviceID = block  */
        case 0x00c: return 0x554d4551;               /* VendorID "QEMU"   */
        case 0x010:                                  /* DeviceFeatures    */
            return v->dev_feat_sel == 1 ? (u32)(v->features >> 32)
                                        : (u32)(v->features & 0xffffffff);
        case 0x034:                                  /* QueueNumMax       */
            return v->queue_sel == 0 ? QUEUE_NUM_MAX : 0;
        case 0x044: return v->q_ready;               /* QueueReady        */
        case 0x060: return v->isr;                   /* InterruptStatus   */
        case 0x070: return v->status;                /* Status            */
        case 0x0fc: return 0;                        /* ConfigGeneration  */
        default:    return 0;
    }
}

static void blk_write(void *opaque, u64 off, unsigned size, u64 val) {
    VirtIOBlk *v = opaque;
    u32 v32 = (u32)val;
    switch (off) {
        case 0x014: v->dev_feat_sel = v32; break;                   /* DeviceFeaturesSel */
        case 0x020:                                                 /* DriverFeatures    */
            if (v->drv_feat_sel == 1) v->drv_feat = (v->drv_feat & 0xffffffffULL) | ((u64)v32 << 32);
            else                      v->drv_feat = (v->drv_feat & ~0xffffffffULL) | v32;
            break;
        case 0x024: v->drv_feat_sel = v32; break;                   /* DriverFeaturesSel */
        case 0x030: v->queue_sel = v32; break;                      /* QueueSel          */
        case 0x038:                                                 /* QueueNum          */
            v->q_num = v32 > QUEUE_NUM_MAX ? QUEUE_NUM_MAX : v32;
            break;
        case 0x044: v->q_ready = v32; break;                        /* QueueReady        */
        case 0x050: if (v32 == 0) virtio_blk_process(v); break;     /* QueueNotify       */
        case 0x064:                                                 /* InterruptACK      */
            v->isr &= ~v32;
            if (v->isr == 0) gic_set_irq(v->gic, v->irq, 0);
            break;
        case 0x070:                                                 /* Status            */
            if (v32 == 0) blk_reset(v); else v->status = v32;
            break;
        case 0x080: v->q_desc  = (v->q_desc  & ~0xffffffffULL) | v32;            break;
        case 0x084: v->q_desc  = (v->q_desc  &  0xffffffffULL) | ((u64)v32 << 32); break;
        case 0x090: v->q_avail = (v->q_avail & ~0xffffffffULL) | v32;            break;
        case 0x094: v->q_avail = (v->q_avail &  0xffffffffULL) | ((u64)v32 << 32); break;
        case 0x0a0: v->q_used  = (v->q_used  & ~0xffffffffULL) | v32;            break;
        case 0x0a4: v->q_used  = (v->q_used  &  0xffffffffULL) | ((u64)v32 << 32); break;
        default: break;
    }
}

struct VirtIOBlk *virtio_blk_create(Machine *m, GIC *gic, const char *path, bool ro, int slot) {
    int fd = open(path, ro ? O_RDONLY : O_RDWR);
    if (fd < 0) { fprintf(stderr, "virtio-blk: cannot open %s\n", path); exit(1); }
    struct stat st;
    if (fstat(fd, &st) != 0) { fprintf(stderr, "virtio-blk: fstat %s\n", path); exit(1); }

    u64 base = 0x0a000000ULL + (u64)slot * 0x200;
    VirtIOBlk *v = calloc(1, sizeof(*v));
    v->m = m; v->gic = gic; v->irq = INTID_VIRTIO0 + slot;
    v->index = m->n_blk;
    v->fd = fd;
    v->ro = ro;
    v->features = BLK_FEATURES | (ro ? VIRTIO_BLK_F_RO : 0);
    v->capacity = (u64)st.st_size / SECTOR_SIZE;
    machine_add_device(m, base, 0x200, blk_read, blk_write, v, "virtio-blk");
    m->blk[m->n_blk++] = v;
    fprintf(stderr, "[virtio-blk] slot %d: %s: %llu sectors (%llu MiB)%s\n", slot, path,
            (unsigned long long)v->capacity, (unsigned long long)((u64)st.st_size >> 20),
            ro ? " [ro]" : "");
    return v;
}
