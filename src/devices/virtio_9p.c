/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* virtio-9p host directory share over a modern (version 2) virtio-mmio
 * transport. Exports a host directory tree into the guest as a 9P2000.L
 * filesystem; the guest mounts it with
 *
 *     mount -t 9p -o trans=virtio,version=9p2000.L <tag> /mnt/point
 *
 * One device == one share == one MMIO slot with its own mount tag (advertised
 * via the VIRTIO_9P_MOUNT_TAG feature + config space). Multiple -virtfs options
 * create multiple independent devices.
 *
 * The device speaks 9P2000.L (the Linux dialect): its messages map 1:1 onto
 * Linux syscalls, and since both guest and host are Linux/arm64 the O_* open
 * flags and errno values pass straight through. Every request is serviced
 * synchronously inside QueueNotify — the guest isn't running while we walk the
 * queue — and the queue interrupt is raised inline, exactly like virtio-blk.
 *
 * The virtio-mmio register layer mirrors virtio_blk.c (single request queue).
 * The bulk of this file is the 9P2000.L protocol engine: a fid table, path
 * containment, message (un)marshalling, and per-opcode handlers backed by
 * openat/readdir/pread/pwrite/… on the host.
 *
 * Security note: `..` is clamped lexically at the share root, but symlinks
 * inside the share are followed by the host, so a share is a convenience, not a
 * security sandbox (matching QEMU's local security_model=none). `,ro` maps the
 * whole tree read-only. */
#include "../devices.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/sysmacros.h>
#include <time.h>

/* ---- virtio-mmio transport constants (shared with blk/net) ---- */
#define VIRTIO_F_VERSION_1    (1ULL << 32)
#define VIRTIO_9P_MOUNT_TAG   (1ULL << 0)
#define FS_FEATURES           (VIRTIO_F_VERSION_1 | VIRTIO_9P_MOUNT_TAG)

#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2

#define QUEUE_NUM_MAX 256
#define VIRTQ_MAX     512          /* descriptor-chain walk bound */

/* Largest 9P message we support; also the size of the per-device gather/scatter
 * buffers. The negotiated msize (Tversion) is clamped to this. */
#define P9_MSIZE_MAX  (128 * 1024)

#define TAG_MAX 128

/* ---- 9P2000.L message types ---- */
enum {
    P9_RLERROR      = 7,
    P9_TSTATFS      = 8,  P9_RSTATFS      = 9,
    P9_TLOPEN       = 12, P9_RLOPEN       = 13,
    P9_TLCREATE     = 14, P9_RLCREATE     = 15,
    P9_TSYMLINK     = 16, P9_RSYMLINK     = 17,
    P9_TMKNOD       = 18, P9_RMKNOD       = 19,
    P9_TRENAME      = 20, P9_RRENAME      = 21,
    P9_TREADLINK    = 22, P9_RREADLINK    = 23,
    P9_TGETATTR     = 24, P9_RGETATTR     = 25,
    P9_TSETATTR     = 26, P9_RSETATTR     = 27,
    P9_TXATTRWALK   = 30, P9_RXATTRWALK   = 31,
    P9_TXATTRCREATE = 32, P9_RXATTRCREATE = 33,
    P9_TREADDIR     = 40, P9_RREADDIR     = 41,
    P9_TFSYNC       = 50, P9_RFSYNC       = 51,
    P9_TLOCK        = 52, P9_RLOCK        = 53,
    P9_TGETLOCK     = 54, P9_RGETLOCK     = 55,
    P9_TLINK        = 70, P9_RLINK        = 71,
    P9_TMKDIR       = 72, P9_RMKDIR       = 73,
    P9_TRENAMEAT    = 74, P9_RRENAMEAT    = 75,
    P9_TUNLINKAT    = 76, P9_RUNLINKAT    = 77,
    P9_TVERSION     = 100, P9_RVERSION    = 101,
    P9_TATTACH      = 104, P9_RATTACH     = 105,
    P9_TFLUSH       = 108, P9_RFLUSH      = 109,
    P9_TWALK        = 110, P9_RWALK       = 111,
    P9_TREAD        = 116, P9_RREAD       = 117,
    P9_TWRITE       = 118, P9_RWRITE      = 119,
    P9_TCLUNK       = 120, P9_RCLUNK      = 121,
    P9_TREMOVE      = 122, P9_RREMOVE     = 123,
};

/* qid.type */
#define P9_QTDIR     0x80
#define P9_QTSYMLINK 0x02
#define P9_QTFILE    0x00

/* Tgetattr / Rgetattr valid mask (subset we fill: mode..blocks, uid/gid, ino). */
#define P9_STATS_BASIC 0x000007ffULL

/* Tsetattr valid bits. */
#define P9_ATTR_MODE      (1u << 0)
#define P9_ATTR_UID       (1u << 1)
#define P9_ATTR_GID       (1u << 2)
#define P9_ATTR_SIZE      (1u << 3)
#define P9_ATTR_ATIME     (1u << 4)
#define P9_ATTR_MTIME     (1u << 5)
#define P9_ATTR_CTIME     (1u << 6)
#define P9_ATTR_ATIME_SET (1u << 7)
#define P9_ATTR_MTIME_SET (1u << 8)

#define P9_MAXWELEM 16            /* max walk elements per Twalk */
#define P9_NAME_MAX 512

/* ---- fid table ---- */
#define FID_BUCKETS 1024
typedef struct P9Fid {
    u32   fid;
    struct P9Fid *next;
    int   fd;                     /* -1 unless opened as a file (Tlopen/Tlcreate) */
    DIR  *dir;                    /* non-NULL when opened as a directory */
    char  path[PATH_MAX];         /* host path, kept inside the share root */
} P9Fid;

typedef struct VirtIO9P {
    Machine *m; GIC *gic; int irq;

    char   root[PATH_MAX]; size_t rootlen;   /* realpath'd share root */
    bool   ro;
    char   tag[TAG_MAX]; u16 tag_len;

    /* virtio-mmio state (single request queue) */
    u32 status, isr;
    u32 dev_feat_sel, drv_feat_sel; u64 drv_feat;
    u32 queue_sel;
    u32 q_num, q_ready;
    u64 q_desc, q_avail, q_used;
    u16 last_avail;

    u32 msize;                    /* negotiated in Tversion */
    u8 *req, *resp;               /* P9_MSIZE_MAX gather/scatter buffers */

    P9Fid *fids[FID_BUCKETS];
} VirtIO9P;

/* ============================ marshalling ============================ */

typedef struct { u8 *b; u32 cap; u32 pos; bool err; } Cur;

static u8  c_u8 (Cur *c) { if (c->pos + 1 > c->cap) { c->err = true; return 0; } return c->b[c->pos++]; }
static u16 c_u16(Cur *c) { u16 v = c_u8(c); v |= (u16)c_u8(c) << 8; return v; }
static u32 c_u32(Cur *c) { u32 v = c_u16(c); v |= (u32)c_u16(c) << 16; return v; }
static u64 c_u64(Cur *c) { u64 v = c_u32(c); v |= (u64)c_u32(c) << 32; return v; }

/* Read a 9P string (u16 len + bytes) into a NUL-terminated buffer. Advances the
 * cursor by the full declared length even when the copy is truncated. */
static bool c_str(Cur *c, char *out, size_t outsz) {
    u16 len = c_u16(c);
    if (c->err) { if (outsz) out[0] = '\0'; return false; }
    if ((u64)c->pos + len > c->cap) { c->err = true; if (outsz) out[0] = '\0'; return false; }
    size_t n = len < outsz - 1 ? len : outsz - 1;
    memcpy(out, c->b + c->pos, n); out[n] = '\0';
    c->pos += len;
    return true;
}

static void p_u8 (Cur *c, u8  v) { if (c->pos + 1 > c->cap) { c->err = true; return; } c->b[c->pos++] = v; }
static void p_u16(Cur *c, u16 v) { p_u8(c, (u8)v); p_u8(c, (u8)(v >> 8)); }
static void p_u32(Cur *c, u32 v) { p_u16(c, (u16)v); p_u16(c, (u16)(v >> 16)); }
static void p_u64(Cur *c, u64 v) { p_u32(c, (u32)v); p_u32(c, (u32)(v >> 32)); }
static void p_str(Cur *c, const char *s) {
    size_t n = strlen(s); if (n > 0xffff) n = 0xffff;
    p_u16(c, (u16)n);
    if (c->pos + n > c->cap) { c->err = true; return; }
    memcpy(c->b + c->pos, s, n); c->pos += (u32)n;
}
static void p_qid(Cur *c, u8 type, u32 ver, u64 path) { p_u8(c, type); p_u32(c, ver); p_u64(c, path); }

/* Patch the 4-byte size prefix (LE) after a response is fully built. */
static void patch_size(u8 *buf, u32 sz) {
    buf[0] = (u8)sz; buf[1] = (u8)(sz >> 8); buf[2] = (u8)(sz >> 16); buf[3] = (u8)(sz >> 24);
}

/* Rlerror(ecode) — the 9P2000.L error reply. errno passes straight through. */
static u32 mk_lerror(VirtIO9P *v, u16 tag, int ec, u32 cap) {
    Cur c = { v->resp, cap < 11 ? 11 : cap, 0, false };
    p_u32(&c, 0); p_u8(&c, P9_RLERROR); p_u16(&c, tag); p_u32(&c, (u32)ec);
    patch_size(v->resp, c.pos);
    return c.pos;
}

static Cur resp_begin(VirtIO9P *v, u8 type, u16 tag, u32 cap) {
    Cur c = { v->resp, cap, 0, false };
    p_u32(&c, 0); p_u8(&c, type); p_u16(&c, tag);
    return c;
}
static u32 resp_finish(Cur *o, VirtIO9P *v, u16 tag, u32 cap) {
    if (o->err) return mk_lerror(v, tag, EMSGSIZE, cap);
    patch_size(v->resp, o->pos);
    return o->pos;
}

/* ============================ qid / paths ============================ */

/* Stable 64-bit qid.path from (st_dev, st_ino). */
static u64 qid_mix(u64 dev, u64 ino) {
    u64 h = dev * 0x9E3779B97F4A7C15ULL;
    h ^= ino + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h ? h : 1;
}
static u8 qid_type(mode_t mode) {
    if (S_ISDIR(mode))  return P9_QTDIR;
    if (S_ISLNK(mode))  return P9_QTSYMLINK;
    return P9_QTFILE;
}
static int qid_of(const char *path, u8 *type, u32 *ver, u64 *qpath) {
    struct stat st;
    if (lstat(path, &st)) return -1;
    *type = qid_type(st.st_mode);
    *ver = 0;
    *qpath = qid_mix((u64)st.st_dev, (u64)st.st_ino);
    return 0;
}

/* Resolve a single path component against `base`, keeping the result inside the
 * share root. Rejects empty names and names containing '/'; clamps ".." at root.
 * Returns 0 on success (out filled) or -1 with errno set. */
static int path_child(VirtIO9P *v, const char *base, const char *name,
                      char *out, size_t outsz) {
    if (name[0] == '\0' || strchr(name, '/')) { errno = EINVAL; return -1; }
    if (!strcmp(name, ".")) {
        if ((size_t)snprintf(out, outsz, "%s", base) >= outsz) { errno = ENAMETOOLONG; return -1; }
        return 0;
    }
    if (!strcmp(name, "..")) {
        if (!strcmp(base, v->root)) { snprintf(out, outsz, "%s", v->root); return 0; }
        char tmp[PATH_MAX]; snprintf(tmp, sizeof(tmp), "%s", base);
        char *slash = strrchr(tmp, '/');
        if (slash) *slash = '\0';
        if (strlen(tmp) < v->rootlen) snprintf(out, outsz, "%s", v->root);
        else                          snprintf(out, outsz, "%s", tmp);
        return 0;
    }
    if ((size_t)snprintf(out, outsz, "%s/%s", base, name) >= outsz) { errno = ENAMETOOLONG; return -1; }
    /* Belt-and-suspenders: the result must still be under the root. */
    if (strncmp(out, v->root, v->rootlen) != 0 ||
        (out[v->rootlen] != '/' && out[v->rootlen] != '\0')) { errno = EACCES; return -1; }
    return 0;
}

/* ============================ fid table ============================ */

static P9Fid *fid_find(VirtIO9P *v, u32 fid) {
    for (P9Fid *f = v->fids[fid & (FID_BUCKETS - 1)]; f; f = f->next)
        if (f->fid == fid) return f;
    return NULL;
}
static P9Fid *fid_new(VirtIO9P *v, u32 fid, const char *path) {
    P9Fid *f = fid_find(v, fid);
    if (f) {                                   /* reuse: drop any open handles */
        if (f->fd >= 0) close(f->fd);
        if (f->dir) closedir(f->dir);
        f->fd = -1; f->dir = NULL;
    } else {
        f = calloc(1, sizeof(*f));
        f->fid = fid; f->fd = -1;
        u32 b = fid & (FID_BUCKETS - 1);
        f->next = v->fids[b]; v->fids[b] = f;
    }
    snprintf(f->path, sizeof(f->path), "%s", path);
    return f;
}
static void fid_del(VirtIO9P *v, u32 fid) {
    P9Fid **pp = &v->fids[fid & (FID_BUCKETS - 1)];
    while (*pp) {
        if ((*pp)->fid == fid) {
            P9Fid *f = *pp; *pp = f->next;
            if (f->fd >= 0) close(f->fd);
            if (f->dir) closedir(f->dir);
            free(f);
            return;
        }
        pp = &(*pp)->next;
    }
}
static void fids_clear(VirtIO9P *v) {
    for (int i = 0; i < FID_BUCKETS; i++) {
        P9Fid *f = v->fids[i];
        while (f) { P9Fid *n = f->next; if (f->fd >= 0) close(f->fd); if (f->dir) closedir(f->dir); free(f); f = n; }
        v->fids[i] = NULL;
    }
}

/* Map 9P2000.L open flags (== Linux asm-generic O_* values) to host O_*, keeping
 * only the safe/portable bits (drops O_DIRECT/O_SYNC/O_DIRECTORY/…). */
static int dotl_to_host(u32 fl) {
    int h;
    switch (fl & 3) { case 1: h = O_WRONLY; break; case 2: case 3: h = O_RDWR; break; default: h = O_RDONLY; }
    if (fl & 000000100) h |= O_CREAT;
    if (fl & 000000200) h |= O_EXCL;
    if (fl & 000001000) h |= O_TRUNC;
    if (fl & 000002000) h |= O_APPEND;
    if (fl & 000004000) h |= O_NONBLOCK;
    if (fl & 000400000) h |= O_NOFOLLOW;
    return h;
}
static bool dotl_wants_write(u32 fl) {
    return (fl & 3) == 1 || (fl & 3) == 2 || (fl & (000000100 | 000001000));
}

/* ============================ handlers ============================ */

static u32 h_version(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 msz = c_u32(in);
    char ver[32]; c_str(in, ver, sizeof(ver));
    fids_clear(v);                             /* Tversion aborts all fids/IO */
    v->msize = msz < P9_MSIZE_MAX ? msz : P9_MSIZE_MAX;
    if (v->msize < 4096) v->msize = 4096;
    const char *rver = !strncmp(ver, "9P2000.L", 8) ? "9P2000.L" : "unknown";
    Cur o = resp_begin(v, P9_RVERSION, tag, cap);
    p_u32(&o, v->msize); p_str(&o, rver);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_attach(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in); (void)c_u32(in);      /* afid ignored */
    char uname[128], aname[256];
    c_str(in, uname, sizeof(uname)); c_str(in, aname, sizeof(aname));
    (void)c_u32(in);                           /* n_uname ignored */
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    u8 qt; u32 qv; u64 qp;
    if (qid_of(v->root, &qt, &qv, &qp)) return mk_lerror(v, tag, errno, cap);
    fid_new(v, fid, v->root);
    Cur o = resp_begin(v, P9_RATTACH, tag, cap);
    p_qid(&o, qt, qv, qp);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_walk(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in), newfid = c_u32(in);
    u16 nw = c_u16(in);
    if (in->err || nw > P9_MAXWELEM) return mk_lerror(v, tag, EINVAL, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);

    char names[P9_MAXWELEM][P9_NAME_MAX];
    for (u16 i = 0; i < nw; i++) c_str(in, names[i], sizeof(names[i]));
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);

    char cur[PATH_MAX]; snprintf(cur, sizeof(cur), "%s", f->path);
    u8 qt[P9_MAXWELEM]; u32 qv[P9_MAXWELEM]; u64 qp[P9_MAXWELEM];
    u16 nqid = 0;
    for (u16 i = 0; i < nw; i++) {
        char child[PATH_MAX];
        if (path_child(v, cur, names[i], child, sizeof(child)) < 0) {
            if (i == 0) return mk_lerror(v, tag, errno, cap);
            break;
        }
        if (qid_of(child, &qt[i], &qv[i], &qp[i])) {
            if (i == 0) return mk_lerror(v, tag, errno, cap);
            break;
        }
        snprintf(cur, sizeof(cur), "%s", child);
        nqid++;
    }
    if (nqid == nw) {                          /* full walk: (re)assign newfid */
        if (newfid == fid) {
            if (f->fd >= 0) { close(f->fd); f->fd = -1; }
            if (f->dir) { closedir(f->dir); f->dir = NULL; }
            snprintf(f->path, sizeof(f->path), "%s", cur);
        } else {
            fid_new(v, newfid, cur);
        }
    }
    Cur o = resp_begin(v, P9_RWALK, tag, cap);
    p_u16(&o, nqid);
    for (u16 i = 0; i < nqid; i++) p_qid(&o, qt[i], qv[i], qp[i]);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_getattr(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in); (void)c_u64(in);      /* request_mask ignored */
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    struct stat st;
    if (lstat(f->path, &st)) return mk_lerror(v, tag, errno, cap);
    Cur o = resp_begin(v, P9_RGETATTR, tag, cap);
    p_u64(&o, P9_STATS_BASIC);
    p_qid(&o, qid_type(st.st_mode), 0, qid_mix((u64)st.st_dev, (u64)st.st_ino));
    p_u32(&o, (u32)st.st_mode);
    p_u32(&o, (u32)st.st_uid);
    p_u32(&o, (u32)st.st_gid);
    p_u64(&o, (u64)st.st_nlink);
    p_u64(&o, (u64)st.st_rdev);
    p_u64(&o, (u64)st.st_size);
    p_u64(&o, (u64)st.st_blksize);
    p_u64(&o, (u64)st.st_blocks);
    p_u64(&o, (u64)st.st_atim.tv_sec); p_u64(&o, (u64)st.st_atim.tv_nsec);
    p_u64(&o, (u64)st.st_mtim.tv_sec); p_u64(&o, (u64)st.st_mtim.tv_nsec);
    p_u64(&o, (u64)st.st_ctim.tv_sec); p_u64(&o, (u64)st.st_ctim.tv_nsec);
    p_u64(&o, 0); p_u64(&o, 0);                /* btime */
    p_u64(&o, 0);                              /* gen */
    p_u64(&o, 0);                              /* data_version */
    return resp_finish(&o, v, tag, cap);
}

static u32 h_setattr(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in), valid = c_u32(in);
    u32 mode = c_u32(in), uid = c_u32(in), gid = c_u32(in);
    u64 size = c_u64(in);
    u64 at_s = c_u64(in), at_ns = c_u64(in), mt_s = c_u64(in), mt_ns = c_u64(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    if (v->ro) return mk_lerror(v, tag, EROFS, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);

    if (valid & P9_ATTR_MODE)
        if (chmod(f->path, mode & 07777)) return mk_lerror(v, tag, errno, cap);
    if (valid & (P9_ATTR_UID | P9_ATTR_GID)) {
        uid_t u = (valid & P9_ATTR_UID) ? (uid_t)uid : (uid_t)-1;
        gid_t g = (valid & P9_ATTR_GID) ? (gid_t)gid : (gid_t)-1;
        if (lchown(f->path, u, g) && errno != EPERM) return mk_lerror(v, tag, errno, cap);
    }
    if (valid & P9_ATTR_SIZE)
        if (truncate(f->path, (off_t)size)) return mk_lerror(v, tag, errno, cap);
    if (valid & (P9_ATTR_ATIME | P9_ATTR_MTIME)) {
        struct timespec ts[2];
        if (valid & P9_ATTR_ATIME)
            ts[0] = (valid & P9_ATTR_ATIME_SET) ? (struct timespec){ (time_t)at_s, (long)at_ns }
                                                : (struct timespec){ 0, UTIME_NOW };
        else ts[0] = (struct timespec){ 0, UTIME_OMIT };
        if (valid & P9_ATTR_MTIME)
            ts[1] = (valid & P9_ATTR_MTIME_SET) ? (struct timespec){ (time_t)mt_s, (long)mt_ns }
                                                : (struct timespec){ 0, UTIME_NOW };
        else ts[1] = (struct timespec){ 0, UTIME_OMIT };
        if (utimensat(AT_FDCWD, f->path, ts, AT_SYMLINK_NOFOLLOW))
            return mk_lerror(v, tag, errno, cap);
    }
    Cur o = resp_begin(v, P9_RSETATTR, tag, cap);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_readdir(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in); u64 off = c_u64(in); u32 count = c_u32(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    if (!f->dir) { f->dir = opendir(f->path); if (!f->dir) return mk_lerror(v, tag, errno, cap); }
    if (off == 0) rewinddir(f->dir); else seekdir(f->dir, (long)off);

    Cur o = resp_begin(v, P9_RREADDIR, tag, cap);
    u32 count_pos = o.pos; p_u32(&o, 0);       /* placeholder data count */
    u32 data_start = o.pos;

    for (;;) {
        long here = telldir(f->dir);
        errno = 0;
        struct dirent *de = readdir(f->dir);
        if (!de) break;
        const char *name = de->d_name;
        u32 esz = 13 + 8 + 1 + 2 + (u32)strlen(name);
        if ((o.pos - data_start) + esz > count || o.pos + esz > o.cap) {
            seekdir(f->dir, here);             /* doesn't fit: leave it for next call */
            break;
        }
        u8 qt = P9_QTFILE, tbyte = de->d_type; u64 qp;
        char child[PATH_MAX]; struct stat st;
        int need = snprintf(child, sizeof(child), "%s/%s", f->path, name);
        if (need > 0 && (size_t)need < sizeof(child) && lstat(child, &st) == 0) {
            qt = qid_type(st.st_mode);
            qp = qid_mix((u64)st.st_dev, (u64)st.st_ino);
            tbyte = (u8)((st.st_mode & 0170000) >> 12);
        } else {
            qp = qid_mix(0, (u64)de->d_ino);
        }
        p_qid(&o, qt, 0, qp);
        p_u64(&o, (u64)telldir(f->dir));       /* offset to resume after this entry */
        p_u8(&o, tbyte);
        p_str(&o, name);
    }
    patch_size(v->resp + count_pos, o.pos - data_start);  /* backfill data count */
    return resp_finish(&o, v, tag, cap);
}

static u32 h_lopen(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in), fl = c_u32(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    struct stat st;
    if (lstat(f->path, &st)) return mk_lerror(v, tag, errno, cap);
    if (v->ro && dotl_wants_write(fl)) return mk_lerror(v, tag, EROFS, cap);
    if (S_ISDIR(st.st_mode)) {
        if (f->dir) closedir(f->dir);
        f->dir = opendir(f->path);
        if (!f->dir) return mk_lerror(v, tag, errno, cap);
    } else {
        int fd = open(f->path, dotl_to_host(fl), 0666);
        if (fd < 0) return mk_lerror(v, tag, errno, cap);
        if (f->fd >= 0) close(f->fd);
        f->fd = fd;
    }
    Cur o = resp_begin(v, P9_RLOPEN, tag, cap);
    p_qid(&o, qid_type(st.st_mode), 0, qid_mix((u64)st.st_dev, (u64)st.st_ino));
    p_u32(&o, 0);                              /* iounit 0 => use msize */
    return resp_finish(&o, v, tag, cap);
}

static u32 h_lcreate(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in);
    char name[P9_NAME_MAX]; c_str(in, name, sizeof(name));
    u32 fl = c_u32(in), mode = c_u32(in); (void)c_u32(in);  /* gid */
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    if (v->ro) return mk_lerror(v, tag, EROFS, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    char child[PATH_MAX];
    if (path_child(v, f->path, name, child, sizeof(child)) < 0) return mk_lerror(v, tag, errno, cap);
    int fd = open(child, dotl_to_host(fl) | O_CREAT, mode & 07777);
    if (fd < 0) return mk_lerror(v, tag, errno, cap);
    struct stat st; fstat(fd, &st);
    if (f->fd >= 0) close(f->fd);
    if (f->dir) { closedir(f->dir); f->dir = NULL; }
    f->fd = fd;
    snprintf(f->path, sizeof(f->path), "%s", child);   /* fid now refers to the new file */
    Cur o = resp_begin(v, P9_RLCREATE, tag, cap);
    p_qid(&o, qid_type(st.st_mode), 0, qid_mix((u64)st.st_dev, (u64)st.st_ino));
    p_u32(&o, 0);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_read(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in); u64 off = c_u64(in); u32 count = c_u32(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f || f->fd < 0) return mk_lerror(v, tag, EBADF, cap);
    u32 room = cap > 11 ? cap - 11 : 0;        /* Rread header = size+type+tag+count = 11 */
    if (count > room) count = room;
    if (count > P9_MSIZE_MAX - 11) count = P9_MSIZE_MAX - 11;
    Cur o = resp_begin(v, P9_RREAD, tag, cap);
    u32 cnt_pos = o.pos; p_u32(&o, 0);
    ssize_t n = pread(f->fd, v->resp + o.pos, count, (off_t)off);
    if (n < 0) return mk_lerror(v, tag, errno, cap);
    o.pos += (u32)n;
    patch_size(v->resp + cnt_pos, (u32)n);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_write(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in); u64 off = c_u64(in); u32 count = c_u32(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    if (v->ro) return mk_lerror(v, tag, EROFS, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f || f->fd < 0) return mk_lerror(v, tag, EBADF, cap);
    if (in->pos + count > in->cap) count = in->cap - in->pos;  /* clamp to gathered data */
    ssize_t n = pwrite(f->fd, v->req + in->pos, count, (off_t)off);
    if (n < 0) return mk_lerror(v, tag, errno, cap);
    Cur o = resp_begin(v, P9_RWRITE, tag, cap);
    p_u32(&o, (u32)n);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_mkdir(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 dfid = c_u32(in);
    char name[P9_NAME_MAX]; c_str(in, name, sizeof(name));
    u32 mode = c_u32(in); (void)c_u32(in);     /* gid */
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    if (v->ro) return mk_lerror(v, tag, EROFS, cap);
    P9Fid *f = fid_find(v, dfid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    char child[PATH_MAX];
    if (path_child(v, f->path, name, child, sizeof(child)) < 0) return mk_lerror(v, tag, errno, cap);
    if (mkdir(child, mode & 07777)) return mk_lerror(v, tag, errno, cap);
    u8 qt; u32 qv; u64 qp;
    if (qid_of(child, &qt, &qv, &qp)) return mk_lerror(v, tag, errno, cap);
    Cur o = resp_begin(v, P9_RMKDIR, tag, cap);
    p_qid(&o, qt, qv, qp);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_mknod(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 dfid = c_u32(in);
    char name[P9_NAME_MAX]; c_str(in, name, sizeof(name));
    u32 mode = c_u32(in), major = c_u32(in), minor = c_u32(in); (void)c_u32(in);  /* gid */
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    if (v->ro) return mk_lerror(v, tag, EROFS, cap);
    P9Fid *f = fid_find(v, dfid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    char child[PATH_MAX];
    if (path_child(v, f->path, name, child, sizeof(child)) < 0) return mk_lerror(v, tag, errno, cap);
    if (mknod(child, mode, makedev(major, minor))) return mk_lerror(v, tag, errno, cap);
    u8 qt; u32 qv; u64 qp;
    if (qid_of(child, &qt, &qv, &qp)) return mk_lerror(v, tag, errno, cap);
    Cur o = resp_begin(v, P9_RMKNOD, tag, cap);
    p_qid(&o, qt, qv, qp);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_symlink(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 dfid = c_u32(in);
    char name[P9_NAME_MAX], target[PATH_MAX];
    c_str(in, name, sizeof(name)); c_str(in, target, sizeof(target));
    (void)c_u32(in);                           /* gid */
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    if (v->ro) return mk_lerror(v, tag, EROFS, cap);
    P9Fid *f = fid_find(v, dfid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    char child[PATH_MAX];
    if (path_child(v, f->path, name, child, sizeof(child)) < 0) return mk_lerror(v, tag, errno, cap);
    if (symlink(target, child)) return mk_lerror(v, tag, errno, cap);
    u8 qt; u32 qv; u64 qp;
    if (qid_of(child, &qt, &qv, &qp)) return mk_lerror(v, tag, errno, cap);
    Cur o = resp_begin(v, P9_RSYMLINK, tag, cap);
    p_qid(&o, qt, qv, qp);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_readlink(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    char buf[PATH_MAX];
    ssize_t n = readlink(f->path, buf, sizeof(buf) - 1);
    if (n < 0) return mk_lerror(v, tag, errno, cap);
    buf[n] = '\0';
    Cur o = resp_begin(v, P9_RREADLINK, tag, cap);
    p_str(&o, buf);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_link(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 dfid = c_u32(in), fid = c_u32(in);
    char name[P9_NAME_MAX]; c_str(in, name, sizeof(name));
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    if (v->ro) return mk_lerror(v, tag, EROFS, cap);
    P9Fid *df = fid_find(v, dfid), *sf = fid_find(v, fid);
    if (!df || !sf) return mk_lerror(v, tag, EBADF, cap);
    char child[PATH_MAX];
    if (path_child(v, df->path, name, child, sizeof(child)) < 0) return mk_lerror(v, tag, errno, cap);
    if (link(sf->path, child)) return mk_lerror(v, tag, errno, cap);
    Cur o = resp_begin(v, P9_RLINK, tag, cap);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_rename(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in), dfid = c_u32(in);
    char name[P9_NAME_MAX]; c_str(in, name, sizeof(name));
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    if (v->ro) return mk_lerror(v, tag, EROFS, cap);
    P9Fid *f = fid_find(v, fid), *df = fid_find(v, dfid);
    if (!f || !df) return mk_lerror(v, tag, EBADF, cap);
    char child[PATH_MAX];
    if (path_child(v, df->path, name, child, sizeof(child)) < 0) return mk_lerror(v, tag, errno, cap);
    if (rename(f->path, child)) return mk_lerror(v, tag, errno, cap);
    snprintf(f->path, sizeof(f->path), "%s", child);   /* fid follows the file */
    Cur o = resp_begin(v, P9_RRENAME, tag, cap);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_renameat(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 od = c_u32(in);
    char oldn[P9_NAME_MAX]; c_str(in, oldn, sizeof(oldn));
    u32 nd = c_u32(in);
    char newn[P9_NAME_MAX]; c_str(in, newn, sizeof(newn));
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    if (v->ro) return mk_lerror(v, tag, EROFS, cap);
    P9Fid *of = fid_find(v, od), *nf = fid_find(v, nd);
    if (!of || !nf) return mk_lerror(v, tag, EBADF, cap);
    char op[PATH_MAX], np[PATH_MAX];
    if (path_child(v, of->path, oldn, op, sizeof(op)) < 0) return mk_lerror(v, tag, errno, cap);
    if (path_child(v, nf->path, newn, np, sizeof(np)) < 0) return mk_lerror(v, tag, errno, cap);
    if (rename(op, np)) return mk_lerror(v, tag, errno, cap);
    Cur o = resp_begin(v, P9_RRENAMEAT, tag, cap);
    return resp_finish(&o, v, tag, cap);
}

#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
static u32 h_unlinkat(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 dfid = c_u32(in);
    char name[P9_NAME_MAX]; c_str(in, name, sizeof(name));
    u32 flags = c_u32(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    if (v->ro) return mk_lerror(v, tag, EROFS, cap);
    P9Fid *f = fid_find(v, dfid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    char child[PATH_MAX];
    if (path_child(v, f->path, name, child, sizeof(child)) < 0) return mk_lerror(v, tag, errno, cap);
    int r = (flags & AT_REMOVEDIR) ? rmdir(child) : unlink(child);
    if (r) return mk_lerror(v, tag, errno, cap);
    Cur o = resp_begin(v, P9_RUNLINKAT, tag, cap);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_remove(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    if (v->ro) { fid_del(v, fid); return mk_lerror(v, tag, EROFS, cap); }
    int r = remove(f->path);                   /* unlink or rmdir as appropriate */
    int e = errno;
    fid_del(v, fid);                           /* Tremove clunks the fid regardless */
    if (r) return mk_lerror(v, tag, e, cap);
    Cur o = resp_begin(v, P9_RREMOVE, tag, cap);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_clunk(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    fid_del(v, fid);
    Cur o = resp_begin(v, P9_RCLUNK, tag, cap);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_fsync(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in), datasync = c_u32(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    if (f->fd >= 0) {
        int r = datasync ? fdatasync(f->fd) : fsync(f->fd);
        if (r) return mk_lerror(v, tag, errno, cap);
    }
    Cur o = resp_begin(v, P9_RFSYNC, tag, cap);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_statfs(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    u32 fid = c_u32(in);
    if (in->err) return mk_lerror(v, tag, EINVAL, cap);
    P9Fid *f = fid_find(v, fid);
    if (!f) return mk_lerror(v, tag, EBADF, cap);
    struct statfs sf;
    if (statfs(f->path, &sf)) return mk_lerror(v, tag, errno, cap);
    u64 fsid = 0; memcpy(&fsid, &sf.f_fsid, sizeof(fsid) < sizeof(sf.f_fsid) ? sizeof(fsid) : sizeof(sf.f_fsid));
    Cur o = resp_begin(v, P9_RSTATFS, tag, cap);
    p_u32(&o, (u32)sf.f_type);
    p_u32(&o, (u32)sf.f_bsize);
    p_u64(&o, (u64)sf.f_blocks);
    p_u64(&o, (u64)sf.f_bfree);
    p_u64(&o, (u64)sf.f_bavail);
    p_u64(&o, (u64)sf.f_files);
    p_u64(&o, (u64)sf.f_ffree);
    p_u64(&o, fsid);
    p_u32(&o, (u32)sf.f_namelen);
    return resp_finish(&o, v, tag, cap);
}

static u32 h_flush(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    (void)c_u16(in);                           /* oldtag: nothing is outstanding */
    Cur o = resp_begin(v, P9_RFLUSH, tag, cap);
    return resp_finish(&o, v, tag, cap);
}

/* xattr is not backed; report "unsupported" so getfattr/setfattr fail cleanly. */
static u32 h_xattr_unsup(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    (void)in; return mk_lerror(v, tag, EOPNOTSUPP, cap);
}

/* POSIX locks: single client, serviced synchronously — grant everything and
 * report no conflicts. */
static u32 h_lock(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    (void)in;
    Cur o = resp_begin(v, P9_RLOCK, tag, cap);
    p_u8(&o, 0);                               /* P9_LOCK_SUCCESS */
    return resp_finish(&o, v, tag, cap);
}
static u32 h_getlock(VirtIO9P *v, Cur *in, u16 tag, u32 cap) {
    (void)c_u32(in); (void)c_u8(in);
    u64 start = c_u64(in), length = c_u64(in); u32 pid = c_u32(in);
    char cid[128]; c_str(in, cid, sizeof(cid));
    Cur o = resp_begin(v, P9_RGETLOCK, tag, cap);
    p_u8(&o, 2);                               /* F_UNLCK: no conflicting lock */
    p_u64(&o, start); p_u64(&o, length); p_u32(&o, pid); p_str(&o, cid);
    return resp_finish(&o, v, tag, cap);
}

/* ============================ dispatch ============================ */

static u32 p9_dispatch(VirtIO9P *v, u32 reqlen, u32 wcap) {
    u32 cap = wcap < P9_MSIZE_MAX ? wcap : P9_MSIZE_MAX;
    Cur in = { v->req, reqlen, 0, false };
    (void)c_u32(&in);                          /* size (use reqlen) */
    u8  type = c_u8(&in);
    u16 tag  = c_u16(&in);
    /* Truncated header: reply Rlerror instead of a zero-length message, which
     * would complete the descriptor with no R-message and hang the tag forever. */
    if (in.err) return mk_lerror(v, tag, EINVAL, cap);

    switch (type) {
        case P9_TVERSION:     return h_version(v, &in, tag, cap);
        case P9_TATTACH:      return h_attach(v, &in, tag, cap);
        case P9_TWALK:        return h_walk(v, &in, tag, cap);
        case P9_TGETATTR:     return h_getattr(v, &in, tag, cap);
        case P9_TSETATTR:     return h_setattr(v, &in, tag, cap);
        case P9_TREADDIR:     return h_readdir(v, &in, tag, cap);
        case P9_TLOPEN:       return h_lopen(v, &in, tag, cap);
        case P9_TLCREATE:     return h_lcreate(v, &in, tag, cap);
        case P9_TREAD:        return h_read(v, &in, tag, cap);
        case P9_TWRITE:       return h_write(v, &in, tag, cap);
        case P9_TMKDIR:       return h_mkdir(v, &in, tag, cap);
        case P9_TMKNOD:       return h_mknod(v, &in, tag, cap);
        case P9_TSYMLINK:     return h_symlink(v, &in, tag, cap);
        case P9_TREADLINK:    return h_readlink(v, &in, tag, cap);
        case P9_TLINK:        return h_link(v, &in, tag, cap);
        case P9_TRENAME:      return h_rename(v, &in, tag, cap);
        case P9_TRENAMEAT:    return h_renameat(v, &in, tag, cap);
        case P9_TUNLINKAT:    return h_unlinkat(v, &in, tag, cap);
        case P9_TREMOVE:      return h_remove(v, &in, tag, cap);
        case P9_TCLUNK:       return h_clunk(v, &in, tag, cap);
        case P9_TFSYNC:       return h_fsync(v, &in, tag, cap);
        case P9_TSTATFS:      return h_statfs(v, &in, tag, cap);
        case P9_TFLUSH:       return h_flush(v, &in, tag, cap);
        case P9_TXATTRWALK:
        case P9_TXATTRCREATE: return h_xattr_unsup(v, &in, tag, cap);
        case P9_TLOCK:        return h_lock(v, &in, tag, cap);
        case P9_TGETLOCK:     return h_getlock(v, &in, tag, cap);
        default:              return mk_lerror(v, tag, EOPNOTSUPP, cap);
    }
}

/* ============================ virtqueue ============================ */

static void push_used(VirtIO9P *v, u32 id, u32 len) {
    Machine *m = v->m;
    u16 used_idx = (u16)phys_read(m, v->q_used + 2, 2);
    u64 e = v->q_used + 4 + (u64)(used_idx % v->q_num) * 8;
    phys_write(m, e + 0, 4, id);
    phys_write(m, e + 4, 4, len);
    phys_write(m, v->q_used + 2, 2, (u16)(used_idx + 1));
}

/* Service one request head: gather readable descriptors into the T-message
 * buffer, dispatch, then scatter the R-message across the writable descriptors. */
static void p9_handle(VirtIO9P *v, u32 head) {
    Machine *m = v->m;
    u64 raddr[VIRTQ_MAX]; u32 rlen[VIRTQ_MAX]; u32 n_r = 0;
    u64 waddr[VIRTQ_MAX]; u32 wlen[VIRTQ_MAX]; u32 n_w = 0;

    u32 idx = head, n = 0;
    for (;;) {
        if (n >= v->q_num || n >= VIRTQ_MAX) break;
        u64 d  = v->q_desc + (u64)idx * 16;
        u64 a  = phys_read(m, d + 0, 8);
        u32 l  = (u32)phys_read(m, d + 8, 4);
        u16 fl = (u16)phys_read(m, d + 12, 2);
        u16 nx = (u16)phys_read(m, d + 14, 2);
        if (fl & VIRTQ_DESC_F_WRITE) { if (n_w < VIRTQ_MAX) { waddr[n_w] = a; wlen[n_w] = l; n_w++; } }
        else                        { if (n_r < VIRTQ_MAX) { raddr[n_r] = a; rlen[n_r] = l; n_r++; } }
        n++;
        if (!(fl & VIRTQ_DESC_F_NEXT)) break;
        idx = nx;
    }

    u32 reqlen = 0;
    for (u32 i = 0; i < n_r && reqlen < P9_MSIZE_MAX; i++) {
        u32 take = rlen[i];
        if (reqlen + take > P9_MSIZE_MAX) take = P9_MSIZE_MAX - reqlen;
        phys_read_blk(m, raddr[i], v->req + reqlen, take);
        reqlen += take;
    }
    u32 wcap = 0; for (u32 i = 0; i < n_w; i++) wcap += wlen[i];

    u32 resplen = p9_dispatch(v, reqlen, wcap);

    u32 off = 0;
    for (u32 i = 0; i < n_w && off < resplen; i++) {
        u32 chunk = wlen[i];
        if (off + chunk > resplen) chunk = resplen - off;
        phys_write_blk(m, waddr[i], v->resp + off, chunk);
        off += chunk;
    }
    push_used(v, head, resplen);
}

static void virtio_9p_process(VirtIO9P *v) {
    Machine *m = v->m;
    if (!v->q_ready || v->q_num == 0) return;
    u16 avail_idx = (u16)phys_read(m, v->q_avail + 2, 2);
    bool did = false;
    while (v->last_avail != avail_idx) {
        u16 hd = (u16)phys_read(m, v->q_avail + 4 + (u64)(v->last_avail % v->q_num) * 2, 2);
        p9_handle(v, hd);
        v->last_avail++;
        did = true;
    }
    if (did) {
        v->isr |= 1;
        gic_set_irq(v->gic, v->irq, 1);
    }
}

/* ============================ MMIO ============================ */

static void fs_reset(VirtIO9P *v) {
    v->status = 0; v->isr = 0;
    v->dev_feat_sel = v->drv_feat_sel = 0; v->drv_feat = 0;
    v->queue_sel = 0;
    v->q_num = 0; v->q_ready = 0;
    v->q_desc = v->q_avail = v->q_used = 0;
    v->last_avail = 0;
    v->msize = P9_MSIZE_MAX;
    fids_clear(v);
    gic_set_irq(v->gic, v->irq, 0);
}

/* struct virtio_9p_config { u16 tag_len; u8 tag[]; } */
static u64 fs_config_read(VirtIO9P *v, u64 off, unsigned size) {
    u8 cfg[2 + TAG_MAX];
    memset(cfg, 0, sizeof(cfg));
    cfg[0] = (u8)v->tag_len;
    cfg[1] = (u8)(v->tag_len >> 8);
    memcpy(cfg + 2, v->tag, v->tag_len);
    u64 r = 0;
    for (unsigned i = 0; i < size && off + i < sizeof(cfg); i++)
        r |= (u64)cfg[off + i] << (i * 8);
    return r;
}

static u64 fs_read(void *opaque, u64 off, unsigned size) {
    VirtIO9P *v = opaque;
    if (off >= 0x100) return fs_config_read(v, off - 0x100, size);
    switch (off) {
        case 0x000: return 0x74726976;               /* MagicValue "virt" */
        case 0x004: return 2;                        /* Version (modern)  */
        case 0x008: return 9;                        /* DeviceID = 9p     */
        case 0x00c: return 0x554d4551;               /* VendorID "QEMU"   */
        case 0x010:                                  /* DeviceFeatures    */
            return v->dev_feat_sel == 1 ? (u32)(FS_FEATURES >> 32)
                                        : (u32)(FS_FEATURES & 0xffffffff);
        case 0x034: return v->queue_sel == 0 ? QUEUE_NUM_MAX : 0;  /* QueueNumMax */
        case 0x044: return v->q_ready;               /* QueueReady        */
        case 0x060: return v->isr;                   /* InterruptStatus   */
        case 0x070: return v->status;                /* Status            */
        case 0x0fc: return 0;                        /* ConfigGeneration  */
        default:    return 0;
    }
}

static void fs_write(void *opaque, u64 off, unsigned size, u64 val) {
    VirtIO9P *v = opaque;
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
        case 0x050: if (v32 == 0) virtio_9p_process(v); break;      /* QueueNotify       */
        case 0x064:                                                 /* InterruptACK      */
            v->isr &= ~v32;
            if (v->isr == 0) gic_set_irq(v->gic, v->irq, 0);
            break;
        case 0x070:                                                 /* Status            */
            if (v32 == 0) fs_reset(v); else v->status = v32;
            break;
        case 0x080: v->q_desc  = (v->q_desc  & ~0xffffffffULL) | v32;             break;
        case 0x084: v->q_desc  = (v->q_desc  &  0xffffffffULL) | ((u64)v32 << 32); break;
        case 0x090: v->q_avail = (v->q_avail & ~0xffffffffULL) | v32;             break;
        case 0x094: v->q_avail = (v->q_avail &  0xffffffffULL) | ((u64)v32 << 32); break;
        case 0x0a0: v->q_used  = (v->q_used  & ~0xffffffffULL) | v32;             break;
        case 0x0a4: v->q_used  = (v->q_used  &  0xffffffffULL) | ((u64)v32 << 32); break;
        default: break;
    }
}

/* ============================ creation ============================ */

struct VirtIO9P *virtio_9p_create(Machine *m, GIC *gic, const char *root,
                                  const char *tag, bool ro, int slot) {
    char real[PATH_MAX];
    if (!realpath(root, real)) {
        fprintf(stderr, "virtio-9p: cannot resolve %s: %s\n", root, strerror(errno));
        exit(1);
    }
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "virtio-9p: %s is not a directory\n", root);
        exit(1);
    }

    VirtIO9P *v = calloc(1, sizeof(*v));
    v->m = m; v->gic = gic; v->irq = INTID_VIRTIO0 + slot; v->ro = ro;
    snprintf(v->root, sizeof(v->root), "%s", real);
    v->rootlen = strlen(v->root);
    size_t tl = strlen(tag); if (tl > TAG_MAX) tl = TAG_MAX;
    memcpy(v->tag, tag, tl); v->tag_len = (u16)tl;
    v->msize = P9_MSIZE_MAX;
    v->req  = malloc(P9_MSIZE_MAX);
    v->resp = malloc(P9_MSIZE_MAX);
    if (!v->req || !v->resp) { fprintf(stderr, "virtio-9p: out of memory\n"); exit(1); }

    u64 base = 0x0a000000ULL + (u64)slot * 0x200;
    machine_add_device(m, base, 0x200, fs_read, fs_write, v, "virtio-9p");
    m->fs[m->n_fs++] = v;
    fprintf(stderr, "[virtio-9p] slot %d: tag \"%s\" -> %s%s\n",
            slot, tag, v->root, ro ? " (ro)" : "");
    return v;
}
