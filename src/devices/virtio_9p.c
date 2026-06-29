/* virtio-9p over modern virtio-mmio.
 *
 * This is a synchronous 9P2000.L server for sharing one host directory with the
 * guest. It intentionally mirrors the simple virtio transport style used by the
 * block and net devices: QueueNotify drains request chains, completes them
 * inline, then raises one used-ring interrupt.
 *
 * The server confines all resolved host paths to the configured root. It follows
 * host symlinks only when their real target remains inside that root. */
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
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>

#define VIRTIO_F_VERSION_1  (1ULL << 32)
#define P9_FEATURES         VIRTIO_F_VERSION_1

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2
#define QUEUE_NUM_MAX       128
#define VIRTQ_MAX           128
#define P9_MAX_MSG          (1024 * 1024)
#define P9_MAX_FIDS         1024

#define P9_TSTATFS          8
#define P9_RSTATFS          9
#define P9_TLOPEN           12
#define P9_RLOPEN           13
#define P9_TLCREATE         14
#define P9_RLCREATE         15
#define P9_TSYMLINK         16
#define P9_RSYMLINK         17
#define P9_TREADLINK        22
#define P9_RREADLINK        23
#define P9_TGETATTR         24
#define P9_RGETATTR         25
#define P9_TSETATTR         26
#define P9_RSETATTR         27
#define P9_TFSYNC           50
#define P9_RFSYNC           51
#define P9_RLERROR          7
#define P9_TVERSION         100
#define P9_RVERSION         101
#define P9_TAUTH            102
#define P9_TATTACH          104
#define P9_RATTACH          105
#define P9_TFLUSH           108
#define P9_RFLUSH           109
#define P9_TWALK            110
#define P9_RWALK            111
#define P9_TREAD            116
#define P9_RREAD            117
#define P9_TWRITE           118
#define P9_RWRITE           119
#define P9_TCLUNK           120
#define P9_RCLUNK           121
#define P9_TREMOVE          122
#define P9_RREMOVE          123
#define P9_TMKDIR           72
#define P9_RMKDIR           73
#define P9_TRENAMEAT        74
#define P9_RRENAMEAT        75
#define P9_TUNLINKAT        76
#define P9_RUNLINKAT        77
#define P9_TREADDIR         40
#define P9_RREADDIR         41

#define P9_QTDIR            0x80
#define P9_QTSYMLINK        0x02
#define P9_NOFID            0xffffffffU

typedef struct {
    u32 fid;
    bool used;
    bool open;
    int fd;
    char rel[PATH_MAX];
} P9Fid;

typedef struct VirtIO9P {
    Machine *m;
    GIC *gic;
    int irq;
    char root[PATH_MAX];
    char tag[64];

    u32 status, isr;
    u32 dev_feat_sel, drv_feat_sel;
    u64 drv_feat;
    u32 queue_sel;
    u32 q_num, q_ready;
    u64 q_desc, q_avail, q_used;
    u16 last_avail;

    u32 msize;
    P9Fid fids[P9_MAX_FIDS];
} VirtIO9P;

typedef struct {
    u8 *p;
    u32 len, off;
} Buf;

static u8 rd8(Buf *b) { return b->off < b->len ? b->p[b->off++] : 0; }
static u16 rd16(Buf *b) { u16 v = 0; for (int i = 0; i < 2; i++) v |= (u16)rd8(b) << (i * 8); return v; }
static u32 rd32(Buf *b) { u32 v = 0; for (int i = 0; i < 4; i++) v |= (u32)rd8(b) << (i * 8); return v; }
static u64 rd64(Buf *b) { u64 v = 0; for (int i = 0; i < 8; i++) v |= (u64)rd8(b) << (i * 8); return v; }

static void wr8(Buf *b, u8 v) { if (b->off < b->len) b->p[b->off] = v; b->off++; }
static void wr16(Buf *b, u16 v) { for (int i = 0; i < 2; i++) wr8(b, (u8)(v >> (i * 8))); }
static void wr32(Buf *b, u32 v) { for (int i = 0; i < 4; i++) wr8(b, (u8)(v >> (i * 8))); }
static void wr64(Buf *b, u64 v) { for (int i = 0; i < 8; i++) wr8(b, (u8)(v >> (i * 8))); }

static bool rdstr(Buf *b, char *dst, size_t cap) {
    u16 n = rd16(b);
    if (b->off + n > b->len || cap == 0) return false;
    size_t copy = n < cap - 1 ? n : cap - 1;
    memcpy(dst, b->p + b->off, copy);
    dst[copy] = 0;
    b->off += n;
    return n < cap;
}

static void wrstr(Buf *b, const char *s) {
    size_t n = strlen(s);
    if (n > 65535) n = 65535;
    wr16(b, (u16)n);
    for (size_t i = 0; i < n; i++) wr8(b, (u8)s[i]);
}

static void p9_reset(VirtIO9P *v) {
    v->status = 0; v->isr = 0;
    v->dev_feat_sel = v->drv_feat_sel = 0; v->drv_feat = 0;
    v->queue_sel = 0;
    v->q_num = v->q_ready = 0;
    v->q_desc = v->q_avail = v->q_used = 0;
    v->last_avail = 0;
    v->msize = 8192;
    for (int i = 0; i < P9_MAX_FIDS; i++) {
        if (v->fids[i].used && v->fids[i].open) close(v->fids[i].fd);
        memset(&v->fids[i], 0, sizeof(v->fids[i]));
        v->fids[i].fd = -1;
    }
    gic_set_irq(v->gic, v->irq, 0);
}

static P9Fid *fid_get(VirtIO9P *v, u32 fid) {
    for (int i = 0; i < P9_MAX_FIDS; i++)
        if (v->fids[i].used && v->fids[i].fid == fid) return &v->fids[i];
    return NULL;
}

static P9Fid *fid_alloc(VirtIO9P *v, u32 fid) {
    P9Fid *old = fid_get(v, fid);
    if (old) {
        if (old->open) close(old->fd);
        memset(old, 0, sizeof(*old));
        old->fd = -1;
        old->fid = fid;
        old->used = true;
        return old;
    }
    for (int i = 0; i < P9_MAX_FIDS; i++) {
        if (!v->fids[i].used) {
            P9Fid *f = &v->fids[i];
            memset(f, 0, sizeof(*f));
            f->fd = -1;
            f->fid = fid;
            f->used = true;
            return f;
        }
    }
    errno = EMFILE;
    return NULL;
}

static void fid_clunk(P9Fid *f) {
    if (f->open) close(f->fd);
    memset(f, 0, sizeof(*f));
    f->fd = -1;
}

static bool under_root(VirtIO9P *v, const char *abs) {
    size_t n = strlen(v->root);
    return !strcmp(abs, v->root) || (!strncmp(abs, v->root, n) && abs[n] == '/');
}

static bool join_rel(const char *base, const char *name, char *out) {
    if (name[0] == '/' || strchr(name, '\0') == NULL) { errno = EINVAL; return false; }
    if (!strcmp(name, ".") || name[0] == 0) {
        snprintf(out, PATH_MAX, "%s", base);
        return true;
    }
    if (!strcmp(name, "..")) {
        snprintf(out, PATH_MAX, "%s", base);
        char *slash = strrchr(out, '/');
        if (slash) *slash = 0; else out[0] = 0;
        return true;
    }
    if (strstr(name, "/../") || !strncmp(name, "../", 3) || strstr(name, "/..")) {
        errno = EPERM;
        return false;
    }
    int n = base[0] ? snprintf(out, PATH_MAX, "%s/%s", base, name)
                    : snprintf(out, PATH_MAX, "%s", name);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return false; }
    return true;
}

static bool abs_existing(VirtIO9P *v, const char *rel, char *abs) {
    char tmp[PATH_MAX], real[PATH_MAX];
    int n = rel[0] ? snprintf(tmp, sizeof(tmp), "%s/%s", v->root, rel)
                   : snprintf(tmp, sizeof(tmp), "%s", v->root);
    if (n < 0 || n >= (int)sizeof(tmp)) { errno = ENAMETOOLONG; return false; }
    if (!realpath(tmp, real)) return false;
    if (!under_root(v, real)) { errno = EPERM; return false; }
    snprintf(abs, PATH_MAX, "%s", real);
    return true;
}

static bool abs_parent(VirtIO9P *v, const char *rel, char *parent, char *name, size_t name_cap) {
    char r[PATH_MAX];
    snprintf(r, sizeof(r), "%s", rel);
    char *slash = strrchr(r, '/');
    if (slash) {
        *slash = 0;
        snprintf(name, name_cap, "%s", slash + 1);
        return abs_existing(v, r, parent);
    }
    snprintf(name, name_cap, "%s", rel);
    return abs_existing(v, "", parent);
}

static bool abs_create_path(VirtIO9P *v, const char *rel, char *abs) {
    char parent[PATH_MAX];
    char name[NAME_MAX + 1];
    if (!abs_parent(v, rel, parent, name, sizeof(name))) return false;
    if (name[0] == 0 || strchr(name, '/')) { errno = EINVAL; return false; }
    int n = snprintf(abs, PATH_MAX, "%s/%s", parent, name);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return false; }
    return true;
}

static u64 qid_path(const struct stat *st) {
    return ((u64)(u32)st->st_dev << 32) ^ (u64)st->st_ino;
}

static u8 qid_type(const struct stat *st) {
    if (S_ISDIR(st->st_mode)) return P9_QTDIR;
    if (S_ISLNK(st->st_mode)) return P9_QTSYMLINK;
    return 0;
}

static void wrqid(Buf *b, const struct stat *st) {
    wr8(b, qid_type(st));
    wr32(b, (u32)st->st_mtime);
    wr64(b, qid_path(st));
}

static int fid_stat(VirtIO9P *v, P9Fid *f, struct stat *st) {
    char abs[PATH_MAX];
    if (!abs_existing(v, f->rel, abs)) return -1;
    return lstat(abs, st);
}

static void reply_hdr(Buf *out, u8 type, u16 tag) {
    out->off = 0;
    wr32(out, 0);
    wr8(out, type);
    wr16(out, tag);
}

static u32 reply_done(Buf *out) {
    u32 n = out->off > out->len ? out->len : out->off;
    out->off = 0;
    wr32(out, n);
    return n;
}

static u32 reply_error(Buf *out, u16 tag, int err) {
    if (err <= 0) err = EIO;
    reply_hdr(out, P9_RLERROR, tag);
    wr32(out, (u32)err);
    return reply_done(out);
}

static int p9_open_flags(u32 flags) {
    int acc = flags & 3;
    int of = acc == 0 ? O_RDONLY : acc == 1 ? O_WRONLY : O_RDWR;
    if (flags & 0100) of |= O_CREAT;
    if (flags & 0200) of |= O_EXCL;
    if (flags & 01000) of |= O_TRUNC;
    if (flags & 02000) of |= O_APPEND;
    return of;
}

static u32 p9_handle(VirtIO9P *v, u8 *req, u32 req_len, u8 *resp, u32 resp_cap) {
    Buf in = { req, req_len, 0 };
    Buf out = { resp, resp_cap, 0 };
    (void)rd32(&in);
    u8 type = rd8(&in);
    u16 tag = rd16(&in);

    if (req_len < 7) return reply_error(&out, tag, EPROTO);

    if (type == P9_TVERSION) {
        u32 msize = rd32(&in);
        char ver[64];
        if (!rdstr(&in, ver, sizeof(ver))) return reply_error(&out, tag, EPROTO);
        v->msize = msize < P9_MAX_MSG ? msize : P9_MAX_MSG;
        reply_hdr(&out, P9_RVERSION, tag);
        wr32(&out, v->msize);
        wrstr(&out, "9P2000.L");
        return reply_done(&out);
    }

    if (type == P9_TATTACH) {
        u32 fid = rd32(&in);
        (void)rd32(&in); /* afid */
        char uname[128], aname[128];
        if (!rdstr(&in, uname, sizeof(uname)) || !rdstr(&in, aname, sizeof(aname)))
            return reply_error(&out, tag, EPROTO);
        P9Fid *f = fid_alloc(v, fid);
        if (!f) return reply_error(&out, tag, errno);
        f->rel[0] = 0;
        struct stat st;
        if (fid_stat(v, f, &st) < 0) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RATTACH, tag);
        wrqid(&out, &st);
        return reply_done(&out);
    }

    if (type == P9_TAUTH) return reply_error(&out, tag, ENOSYS);
    if (type == P9_TFLUSH) { reply_hdr(&out, P9_RFLUSH, tag); return reply_done(&out); }

    if (type == P9_TWALK) {
        u32 fid = rd32(&in), newfid = rd32(&in);
        u16 nwname = rd16(&in);
        P9Fid *src = fid_get(v, fid);
        if (!src) return reply_error(&out, tag, EBADF);
        char rel[PATH_MAX];
        snprintf(rel, sizeof(rel), "%s", src->rel);
        struct stat qst[16];
        if (nwname > 16) return reply_error(&out, tag, E2BIG);
        u16 walked = 0;
        for (u16 i = 0; i < nwname; i++) {
            char name[NAME_MAX + 1], next[PATH_MAX], abs[PATH_MAX];
            if (!rdstr(&in, name, sizeof(name))) return reply_error(&out, tag, EPROTO);
            if (!join_rel(rel, name, next)) break;
            if (!abs_existing(v, next, abs) || lstat(abs, &qst[i]) < 0) break;
            snprintf(rel, sizeof(rel), "%s", next);
            walked++;
        }
        if (walked != nwname && newfid != fid) return reply_error(&out, tag, errno ? errno : ENOENT);
        P9Fid *dst = newfid == fid ? src : fid_alloc(v, newfid);
        if (!dst) return reply_error(&out, tag, errno);
        snprintf(dst->rel, sizeof(dst->rel), "%s", rel);
        reply_hdr(&out, P9_RWALK, tag);
        wr16(&out, walked);
        for (u16 i = 0; i < walked; i++) wrqid(&out, &qst[i]);
        return reply_done(&out);
    }

    if (type == P9_TCLUNK) {
        P9Fid *f = fid_get(v, rd32(&in));
        if (f) fid_clunk(f);
        reply_hdr(&out, P9_RCLUNK, tag);
        return reply_done(&out);
    }

    if (type == P9_TGETATTR) {
        P9Fid *f = fid_get(v, rd32(&in));
        (void)rd64(&in); /* request mask */
        if (!f) return reply_error(&out, tag, EBADF);
        struct stat st;
        if (fid_stat(v, f, &st) < 0) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RGETATTR, tag);
        wr64(&out, 0x3fffULL);
        wrqid(&out, &st);
        wr32(&out, (u32)st.st_mode);
        wr32(&out, (u32)st.st_uid);
        wr32(&out, (u32)st.st_gid);
        wr64(&out, (u64)st.st_nlink);
        wr64(&out, (u64)st.st_rdev);
        wr64(&out, (u64)st.st_size);
        wr64(&out, (u64)st.st_blksize);
        wr64(&out, (u64)st.st_blocks);
        wr64(&out, (u64)st.st_atim.tv_sec); wr64(&out, (u64)st.st_atim.tv_nsec);
        wr64(&out, (u64)st.st_mtim.tv_sec); wr64(&out, (u64)st.st_mtim.tv_nsec);
        wr64(&out, (u64)st.st_ctim.tv_sec); wr64(&out, (u64)st.st_ctim.tv_nsec);
        wr64(&out, 0); wr64(&out, 0);
        wr64(&out, 0); wr64(&out, 0);
        return reply_done(&out);
    }

    if (type == P9_TSTATFS) {
        P9Fid *f = fid_get(v, rd32(&in));
        if (!f) return reply_error(&out, tag, EBADF);
        char abs[PATH_MAX];
        struct statvfs sv;
        if (!abs_existing(v, f->rel, abs) || statvfs(abs, &sv) < 0) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RSTATFS, tag);
        wr32(&out, (u32)sv.f_type);
        wr32(&out, (u32)sv.f_bsize);
        wr64(&out, sv.f_blocks);
        wr64(&out, sv.f_bfree);
        wr64(&out, sv.f_bavail);
        wr64(&out, sv.f_files);
        wr64(&out, sv.f_ffree);
        wr64(&out, 0);
        wr32(&out, (u32)sv.f_namemax);
        return reply_done(&out);
    }

    if (type == P9_TSETATTR) {
        P9Fid *f = fid_get(v, rd32(&in));
        u32 valid = rd32(&in);
        u32 mode = rd32(&in);
        u32 uid = rd32(&in);
        u32 gid = rd32(&in);
        u64 size = rd64(&in);
        u64 atime_sec = rd64(&in), atime_nsec = rd64(&in);
        u64 mtime_sec = rd64(&in), mtime_nsec = rd64(&in);
        if (!f) return reply_error(&out, tag, EBADF);
        char abs[PATH_MAX];
        if (!abs_existing(v, f->rel, abs)) return reply_error(&out, tag, errno);
        if ((valid & 0x1) && chmod(abs, mode & 07777) < 0) return reply_error(&out, tag, errno);
        if ((valid & (0x2 | 0x4)) && chown(abs, (valid & 0x2) ? (uid_t)uid : (uid_t)-1,
                                           (valid & 0x4) ? (gid_t)gid : (gid_t)-1) < 0)
            return reply_error(&out, tag, errno);
        if ((valid & 0x8) && truncate(abs, (off_t)size) < 0) return reply_error(&out, tag, errno);
        if (valid & (0x80 | 0x100)) {
            struct timespec ts[2];
            ts[0].tv_sec = (time_t)atime_sec;
            ts[0].tv_nsec = (valid & 0x80) ? (long)atime_nsec : UTIME_OMIT;
            ts[1].tv_sec = (time_t)mtime_sec;
            ts[1].tv_nsec = (valid & 0x100) ? (long)mtime_nsec : UTIME_OMIT;
            if (utimensat(AT_FDCWD, abs, ts, 0) < 0) return reply_error(&out, tag, errno);
        }
        reply_hdr(&out, P9_RSETATTR, tag);
        return reply_done(&out);
    }

    if (type == P9_TLOPEN) {
        P9Fid *f = fid_get(v, rd32(&in));
        u32 flags = rd32(&in);
        if (!f) return reply_error(&out, tag, EBADF);
        char abs[PATH_MAX];
        if (!abs_existing(v, f->rel, abs)) return reply_error(&out, tag, errno);
        int fd = open(abs, p9_open_flags(flags) | O_CLOEXEC);
        if (fd < 0) return reply_error(&out, tag, errno);
        if (f->open) close(f->fd);
        f->fd = fd; f->open = true;
        struct stat st;
        if (fstat(fd, &st) < 0) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RLOPEN, tag);
        wrqid(&out, &st);
        wr32(&out, 8192);
        return reply_done(&out);
    }

    if (type == P9_TLCREATE) {
        P9Fid *f = fid_get(v, rd32(&in));
        char name[NAME_MAX + 1];
        if (!f || !rdstr(&in, name, sizeof(name))) return reply_error(&out, tag, f ? EPROTO : EBADF);
        u32 flags = rd32(&in), mode = rd32(&in);
        (void)rd32(&in); /* gid */
        char rel[PATH_MAX], abs[PATH_MAX];
        if (!join_rel(f->rel, name, rel) || !abs_create_path(v, rel, abs)) return reply_error(&out, tag, errno);
        int fd = open(abs, p9_open_flags(flags) | O_CREAT | O_CLOEXEC, mode & 07777);
        if (fd < 0) return reply_error(&out, tag, errno);
        if (f->open) close(f->fd);
        f->fd = fd; f->open = true;
        snprintf(f->rel, sizeof(f->rel), "%s", rel);
        struct stat st;
        if (fstat(fd, &st) < 0) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RLCREATE, tag);
        wrqid(&out, &st);
        wr32(&out, 8192);
        return reply_done(&out);
    }

    if (type == P9_TREAD || type == P9_TWRITE) {
        P9Fid *f = fid_get(v, rd32(&in));
        u64 off = rd64(&in);
        u32 count = rd32(&in);
        if (!f) return reply_error(&out, tag, EBADF);
        if (!f->open) {
            char abs[PATH_MAX];
            if (!abs_existing(v, f->rel, abs)) return reply_error(&out, tag, errno);
            int fd = open(abs, type == P9_TREAD ? O_RDONLY | O_CLOEXEC : O_RDWR | O_CLOEXEC);
            if (fd < 0) return reply_error(&out, tag, errno);
            f->fd = fd; f->open = true;
        }
        if (type == P9_TREAD) {
            if (count > resp_cap - 11) count = resp_cap - 11;
            reply_hdr(&out, P9_RREAD, tag);
            u32 pos = out.off;
            wr32(&out, 0);
            ssize_t n = pread(f->fd, out.p + out.off, count, (off_t)off);
            if (n < 0) return reply_error(&out, tag, errno);
            out.off += (u32)n;
            u32 save = out.off; out.off = pos; wr32(&out, (u32)n); out.off = save;
            return reply_done(&out);
        } else {
            if (in.off + count > in.len) return reply_error(&out, tag, EPROTO);
            ssize_t n = pwrite(f->fd, in.p + in.off, count, (off_t)off);
            if (n < 0) return reply_error(&out, tag, errno);
            reply_hdr(&out, P9_RWRITE, tag);
            wr32(&out, (u32)n);
            return reply_done(&out);
        }
    }

    if (type == P9_TFSYNC) {
        P9Fid *f = fid_get(v, rd32(&in));
        (void)rd32(&in);
        if (!f) return reply_error(&out, tag, EBADF);
        if (f->open && fsync(f->fd) < 0) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RFSYNC, tag);
        return reply_done(&out);
    }

    if (type == P9_TREADDIR) {
        P9Fid *f = fid_get(v, rd32(&in));
        u64 off = rd64(&in);
        u32 count = rd32(&in);
        if (!f) return reply_error(&out, tag, EBADF);
        char abs[PATH_MAX];
        if (!abs_existing(v, f->rel, abs)) return reply_error(&out, tag, errno);
        DIR *d = opendir(abs);
        if (!d) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RREADDIR, tag);
        u32 count_pos = out.off;
        wr32(&out, 0);
        u32 start = out.off, idx = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (idx++ < off) continue;
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char rel[PATH_MAX], p[PATH_MAX];
            struct stat st;
            if (!join_rel(f->rel, de->d_name, rel) || !abs_existing(v, rel, p) || lstat(p, &st) < 0) continue;
            u32 before = out.off;
            wrqid(&out, &st);
            wr64(&out, idx);
            wr8(&out, qid_type(&st) == P9_QTDIR ? DT_DIR : DT_REG);
            wrstr(&out, de->d_name);
            if (out.off - start > count || out.off > out.len) { out.off = before; break; }
        }
        closedir(d);
        u32 data_len = out.off - start;
        u32 save = out.off; out.off = count_pos; wr32(&out, data_len); out.off = save;
        return reply_done(&out);
    }

    if (type == P9_TMKDIR) {
        P9Fid *df = fid_get(v, rd32(&in));
        char name[NAME_MAX + 1];
        if (!df || !rdstr(&in, name, sizeof(name))) return reply_error(&out, tag, df ? EPROTO : EBADF);
        u32 mode = rd32(&in);
        (void)rd32(&in);
        char rel[PATH_MAX], abs[PATH_MAX];
        if (!join_rel(df->rel, name, rel) || !abs_create_path(v, rel, abs)) return reply_error(&out, tag, errno);
        if (mkdir(abs, mode & 07777) < 0) return reply_error(&out, tag, errno);
        struct stat st;
        if (lstat(abs, &st) < 0) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RMKDIR, tag);
        wrqid(&out, &st);
        return reply_done(&out);
    }

    if (type == P9_TSYMLINK) {
        P9Fid *df = fid_get(v, rd32(&in));
        char name[NAME_MAX + 1], symtgt[PATH_MAX];
        if (!df || !rdstr(&in, name, sizeof(name)) || !rdstr(&in, symtgt, sizeof(symtgt)))
            return reply_error(&out, tag, df ? EPROTO : EBADF);
        (void)rd32(&in);
        char rel[PATH_MAX], abs[PATH_MAX];
        if (!join_rel(df->rel, name, rel) || !abs_create_path(v, rel, abs)) return reply_error(&out, tag, errno);
        if (symlink(symtgt, abs) < 0) return reply_error(&out, tag, errno);
        struct stat st;
        if (lstat(abs, &st) < 0) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RSYMLINK, tag);
        wrqid(&out, &st);
        return reply_done(&out);
    }

    if (type == P9_TRENAMEAT) {
        P9Fid *olddf = fid_get(v, rd32(&in));
        char oldname[NAME_MAX + 1];
        if (!olddf || !rdstr(&in, oldname, sizeof(oldname))) return reply_error(&out, tag, olddf ? EPROTO : EBADF);
        P9Fid *newdf = fid_get(v, rd32(&in));
        char newname[NAME_MAX + 1];
        if (!newdf || !rdstr(&in, newname, sizeof(newname))) return reply_error(&out, tag, newdf ? EPROTO : EBADF);
        char oldrel[PATH_MAX], newrel[PATH_MAX], oldabs[PATH_MAX], newabs[PATH_MAX];
        if (!join_rel(olddf->rel, oldname, oldrel) || !abs_existing(v, oldrel, oldabs) ||
            !join_rel(newdf->rel, newname, newrel) || !abs_create_path(v, newrel, newabs))
            return reply_error(&out, tag, errno);
        if (rename(oldabs, newabs) < 0) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RRENAMEAT, tag);
        return reply_done(&out);
    }

    if (type == P9_TUNLINKAT) {
        P9Fid *df = fid_get(v, rd32(&in));
        char name[NAME_MAX + 1];
        if (!df || !rdstr(&in, name, sizeof(name))) return reply_error(&out, tag, df ? EPROTO : EBADF);
        u32 flags = rd32(&in);
        char rel[PATH_MAX], abs[PATH_MAX];
        if (!join_rel(df->rel, name, rel) || !abs_existing(v, rel, abs)) return reply_error(&out, tag, errno);
        int rc = (flags & AT_REMOVEDIR) ? rmdir(abs) : unlink(abs);
        if (rc < 0) return reply_error(&out, tag, errno);
        reply_hdr(&out, P9_RUNLINKAT, tag);
        return reply_done(&out);
    }

    if (type == P9_TREMOVE) {
        P9Fid *f = fid_get(v, rd32(&in));
        if (!f) return reply_error(&out, tag, EBADF);
        char abs[PATH_MAX];
        struct stat st;
        if (!abs_existing(v, f->rel, abs) || lstat(abs, &st) < 0) return reply_error(&out, tag, errno);
        int rc = S_ISDIR(st.st_mode) ? rmdir(abs) : unlink(abs);
        if (rc < 0) return reply_error(&out, tag, errno);
        fid_clunk(f);
        reply_hdr(&out, P9_RREMOVE, tag);
        return reply_done(&out);
    }

    if (type == P9_TREADLINK) {
        P9Fid *f = fid_get(v, rd32(&in));
        if (!f) return reply_error(&out, tag, EBADF);
        char abs[PATH_MAX], target[PATH_MAX];
        if (!abs_create_path(v, f->rel, abs)) return reply_error(&out, tag, errno);
        ssize_t n = readlink(abs, target, sizeof(target) - 1);
        if (n < 0) return reply_error(&out, tag, errno);
        target[n] = 0;
        reply_hdr(&out, P9_RREADLINK, tag);
        wrstr(&out, target);
        return reply_done(&out);
    }

    return reply_error(&out, tag, ENOSYS);
}

static void p9_push_used(VirtIO9P *v, u32 id, u32 len) {
    Machine *m = v->m;
    u16 used_idx = (u16)phys_read(m, v->q_used + 2, 2);
    u64 e = v->q_used + 4 + (u64)(used_idx % v->q_num) * 8;
    phys_write(m, e + 0, 4, id);
    phys_write(m, e + 4, 4, len);
    phys_write(m, v->q_used + 2, 2, (u16)(used_idx + 1));
}

static void p9_request(VirtIO9P *v, u32 head) {
    Machine *m = v->m;
    u64 addr[VIRTQ_MAX];
    u32 dlen[VIRTQ_MAX];
    u16 dflags[VIRTQ_MAX];
    u32 n = 0, idx = head;
    for (;;) {
        if (n >= v->q_num) break;
        u64 d = v->q_desc + (u64)idx * 16;
        addr[n] = phys_read(m, d + 0, 8);
        dlen[n] = (u32)phys_read(m, d + 8, 4);
        dflags[n] = (u16)phys_read(m, d + 12, 2);
        u16 next = (u16)phys_read(m, d + 14, 2);
        n++;
        if (!(dflags[n - 1] & VIRTQ_DESC_F_NEXT)) break;
        idx = next;
    }
    if (n == 0) return;

    u32 req_len = 0, resp_cap = 0;
    for (u32 i = 0; i < n; i++) {
        if (dflags[i] & VIRTQ_DESC_F_WRITE) resp_cap += dlen[i];
        else req_len += dlen[i];
    }
    if (req_len > P9_MAX_MSG) req_len = P9_MAX_MSG;
    if (resp_cap > P9_MAX_MSG) resp_cap = P9_MAX_MSG;
    u8 *req = malloc(req_len ? req_len : 1);
    u8 *resp = calloc(1, resp_cap ? resp_cap : 1);
    if (!req || !resp) { free(req); free(resp); return; }

    u32 ro = 0;
    for (u32 i = 0; i < n && ro < req_len; i++) {
        if (dflags[i] & VIRTQ_DESC_F_WRITE) continue;
        u32 chunk = dlen[i];
        if (chunk > req_len - ro) chunk = req_len - ro;
        phys_read_blk(m, addr[i], req + ro, chunk);
        ro += chunk;
    }

    u32 out_len = p9_handle(v, req, ro, resp, resp_cap);
    u32 wo = 0;
    for (u32 i = 0; i < n && wo < out_len; i++) {
        if (!(dflags[i] & VIRTQ_DESC_F_WRITE)) continue;
        u32 chunk = dlen[i];
        if (chunk > out_len - wo) chunk = out_len - wo;
        phys_write_blk(m, addr[i], resp + wo, chunk);
        wo += chunk;
    }

    p9_push_used(v, head, wo);
    free(req);
    free(resp);
}

static void p9_process(VirtIO9P *v) {
    Machine *m = v->m;
    if (!v->q_ready || v->q_num == 0) return;
    u16 avail_idx = (u16)phys_read(m, v->q_avail + 2, 2);
    bool did = false;
    while (v->last_avail != avail_idx) {
        u16 hd = (u16)phys_read(m, v->q_avail + 4 + (u64)(v->last_avail % v->q_num) * 2, 2);
        v->last_avail++;
        p9_request(v, hd);
        did = true;
    }
    if (did) {
        v->isr |= 1;
        gic_set_irq(v->gic, v->irq, 1);
    }
}

static u64 p9_config_read(VirtIO9P *v, u64 off, unsigned size) {
    u8 cfg[2 + sizeof(v->tag)];
    size_t tag_len = strlen(v->tag);
    memset(cfg, 0, sizeof(cfg));
    cfg[0] = (u8)tag_len;
    cfg[1] = (u8)(tag_len >> 8);
    memcpy(cfg + 2, v->tag, tag_len);
    u64 r = 0;
    for (unsigned i = 0; i < size && off + i < sizeof(cfg); i++)
        r |= (u64)cfg[off + i] << (i * 8);
    return r;
}

static u64 p9_read(void *opaque, u64 off, unsigned size) {
    VirtIO9P *v = opaque;
    if (off >= 0x100) return p9_config_read(v, off - 0x100, size);
    switch (off) {
        case 0x000: return 0x74726976;
        case 0x004: return 2;
        case 0x008: return 9;
        case 0x00c: return 0x554d4551;
        case 0x010: return v->dev_feat_sel == 1 ? (u32)(P9_FEATURES >> 32) : (u32)P9_FEATURES;
        case 0x034: return v->queue_sel == 0 ? QUEUE_NUM_MAX : 0;
        case 0x044: return v->q_ready;
        case 0x060: return v->isr;
        case 0x070: return v->status;
        case 0x0fc: return 0;
        default: return 0;
    }
}

static void p9_write(void *opaque, u64 off, unsigned size, u64 val) {
    VirtIO9P *v = opaque;
    u32 v32 = (u32)val;
    switch (off) {
        case 0x014: v->dev_feat_sel = v32; break;
        case 0x020:
            if (v->drv_feat_sel == 1) v->drv_feat = (v->drv_feat & 0xffffffffULL) | ((u64)v32 << 32);
            else v->drv_feat = (v->drv_feat & ~0xffffffffULL) | v32;
            break;
        case 0x024: v->drv_feat_sel = v32; break;
        case 0x030: v->queue_sel = v32; break;
        case 0x038: v->q_num = v32 > QUEUE_NUM_MAX ? QUEUE_NUM_MAX : v32; break;
        case 0x044: v->q_ready = v32; break;
        case 0x050: if (v32 == 0) p9_process(v); break;
        case 0x064:
            v->isr &= ~v32;
            if (v->isr == 0) gic_set_irq(v->gic, v->irq, 0);
            break;
        case 0x070: if (v32 == 0) p9_reset(v); else v->status = v32; break;
        case 0x080: v->q_desc  = (v->q_desc  & ~0xffffffffULL) | v32; break;
        case 0x084: v->q_desc  = (v->q_desc  &  0xffffffffULL) | ((u64)v32 << 32); break;
        case 0x090: v->q_avail = (v->q_avail & ~0xffffffffULL) | v32; break;
        case 0x094: v->q_avail = (v->q_avail &  0xffffffffULL) | ((u64)v32 << 32); break;
        case 0x0a0: v->q_used  = (v->q_used  & ~0xffffffffULL) | v32; break;
        case 0x0a4: v->q_used  = (v->q_used  &  0xffffffffULL) | ((u64)v32 << 32); break;
        default: break;
    }
}

VirtIO9P *virtio_9p_create(Machine *m, GIC *gic, const char *root, const char *tag, int slot) {
    char real[PATH_MAX];
    struct stat st;
    if (!realpath(root, real) || stat(real, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "virtio-9p: %s is not a readable directory\n", root);
        exit(1);
    }
    VirtIO9P *v = calloc(1, sizeof(*v));
    if (!v) { fprintf(stderr, "virtio-9p: out of memory\n"); exit(1); }
    v->m = m;
    v->gic = gic;
    v->irq = INTID_VIRTIO0 + slot;
    snprintf(v->root, sizeof(v->root), "%s", real);
    snprintf(v->tag, sizeof(v->tag), "%s", tag && *tag ? tag : "hostshare");
    v->msize = 8192;
    for (int i = 0; i < P9_MAX_FIDS; i++) v->fids[i].fd = -1;
    machine_add_device(m, 0x0a000000ULL + (u64)slot * 0x200, 0x200,
                       p9_read, p9_write, v, "virtio-9p");
    m->fs9p = v;
    fprintf(stderr, "[virtio-9p] slot %d: tag=%s root=%s\n", slot, v->tag, v->root);
    return v;
}
