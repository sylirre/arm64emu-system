/* virtio-net over modern (version 2) virtio-mmio transport, slot 0 of QEMU's
 * 'virt' map (0x0a000000, INTID 48 / SPI 16, edge-triggered per the DTB).
 *
 * The backend is libslirp: a user-space TCP/IP stack that NATs guest traffic
 * through regular host sockets — no TAP, TUN, or privileges required.
 *
 * Two virtqueues:
 *   queue 0 (RX): guest posts free buffers; we fill them with incoming frames.
 *   queue 1 (TX): guest posts outgoing frames; we pass them to slirp_input().
 *
 * Processing is synchronous: TX is handled on QueueNotify, RX is injected
 * during virtio_net_poll() which is called from machine_tick() every ~1024
 * guest instructions. */
#include "../devices.h"
#include "../net/usernet.h"
#include "libslirp.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>

/* Feature bits. VIRTIO_NET_F_MAC and F_STATUS; VIRTIO_F_VERSION_1 mandatory
 * for a modern device (version 2 transport). No offload features — simplest
 * possible path through the Linux virtio_net driver. */
#define VIRTIO_F_VERSION_1  (1ULL << 32)
#define VIRTIO_NET_F_MAC    (1ULL << 5)
#define VIRTIO_NET_F_STATUS (1ULL << 16)
#define NET_FEATURES        (VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS)

#define VIRTIO_NET_S_LINK_UP  1

/* virtio_net header length. The Linux virtio_net driver (and QEMU) use the
 * 12-byte struct virtio_net_hdr_mrg_rxbuf whenever VIRTIO_F_VERSION_1 OR
 * VIRTIO_NET_F_MRG_RXBUF is negotiated — the bare 10-byte struct virtio_net_hdr
 * is used only by a legacy device that negotiates neither. We are a modern
 * (version-2 transport) device that negotiates VERSION_1, so the guest prepends
 * 12 bytes on TX and expects 12 bytes on RX, even though we don't merge buffers.
 * The extra 2 bytes are num_buffers (set to 1 on RX, ignored on TX). */
#define NET_HDR_LEN   12

/* Split-virtqueue descriptor flags. */
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define QUEUE_NUM_MAX 256
#define VIRTQ_MAX     256

#define NET_QUEUE_RX  0
#define NET_QUEUE_TX  1

/* Pending RX frame ring — frames queued by slirp_output before the guest
 * RX queue is ready. Power-of-two so the head/tail modulo is a mask. */
#define RX_RING_SIZE  64          /* must be power of 2 */
#define MAX_ETH_FRAME 1518

/* Poll fd table used during virtio_net_poll(). */
#define NET_POLL_MAX 128

/* Minimal timer for slirp (used for TCP retransmit, ARP expiry, etc.). */
typedef struct NetTimer {
    SlirpTimerCb     cb;
    void            *cb_opaque;
    int64_t          expire_ms;    /* 0 = not armed */
    struct NetTimer *next;
} NetTimer;

typedef struct VirtIONet {
    Machine *m; GIC *gic; int irq;   /* INTID 49 */
    Slirp   *slirp;                  /* default backend */
    UserNet *un;                     /* AENET=user backend (transitional) */
    NetTimer *timers;

    uint8_t  mac[6];
    uint32_t status, isr;
    uint32_t dev_feat_sel, drv_feat_sel; uint64_t drv_feat;
    uint32_t queue_sel;

    uint32_t q_num[2], q_ready[2];
    uint64_t q_desc[2], q_avail[2], q_used[2];
    uint16_t last_avail[2];

    /* Pending RX frames from slirp (written by slirp_output callback,
     * drained into guest by net_flush_rx). */
    uint8_t  rx_buf[RX_RING_SIZE][NET_HDR_LEN + MAX_ETH_FRAME];
    uint32_t rx_len[RX_RING_SIZE];
    int      rx_head, rx_tail;
} VirtIONet;

/* ---- helpers ---- */

static void net_reset(VirtIONet *v) {
    if (v->un) usernet_guest_reset(v->un);
    v->status = 0; v->isr = 0;
    v->dev_feat_sel = v->drv_feat_sel = 0; v->drv_feat = 0;
    v->queue_sel = 0;
    for (int q = 0; q < 2; q++) {
        v->q_num[q] = v->q_ready[q] = 0;
        v->q_desc[q] = v->q_avail[q] = v->q_used[q] = 0;
        v->last_avail[q] = 0;
    }
    v->rx_head = v->rx_tail = 0;    /* drop frames queued for the old driver */
    gic_set_irq(v->gic, v->irq, 0);
}

/* System-reset entry point: identical to a guest-driven STATUS=0 device reset.
 * Called explicitly on reboot because virtio-net is background-polled, so its
 * queues must be quiesced before the rebooting firmware reuses guest RAM (the
 * network backend and any host port-forwards are preserved). */
void virtio_net_reset(VirtIONet *v) { net_reset(v); }

static void push_used(VirtIONet *v, int q, uint16_t id, uint32_t len) {
    Machine *m = v->m;
    uint16_t ui = (uint16_t)phys_read(m, v->q_used[q] + 2, 2);
    uint64_t e  = v->q_used[q] + 4 + (uint64_t)(ui % v->q_num[q]) * 8;
    phys_write(m, e + 0, 4, id);
    phys_write(m, e + 4, 4, len);
    phys_write(m, v->q_used[q] + 2, 2, (uint16_t)(ui + 1));
}

/* ---- RX: deliver pending frames from the ring into guest buffers ---- */

static void net_flush_rx(VirtIONet *v) {
    Machine *m = v->m;
    if (!v->q_ready[NET_QUEUE_RX] || v->q_num[NET_QUEUE_RX] == 0) return;
    bool did = false;

    while (v->rx_head != v->rx_tail) {
        uint16_t avail_idx = (uint16_t)phys_read(m, v->q_avail[NET_QUEUE_RX] + 2, 2);
        if (v->last_avail[NET_QUEUE_RX] == avail_idx) break; /* no free guest buffers */

        uint16_t q_num = (uint16_t)v->q_num[NET_QUEUE_RX];
        uint16_t hd = (uint16_t)phys_read(m,
            v->q_avail[NET_QUEUE_RX] + 4 +
            (uint64_t)(v->last_avail[NET_QUEUE_RX] % q_num) * 2, 2);
        v->last_avail[NET_QUEUE_RX]++;

        int slot = v->rx_head;
        v->rx_head = (v->rx_head + 1) & (RX_RING_SIZE - 1);

        uint8_t  *frame = v->rx_buf[slot];
        uint32_t  flen  = v->rx_len[slot];

        /* Write the frame (hdr + packet) into the guest descriptor chain.
         * Bound the walk by q_num (a malformed cyclic chain must not hang us)
         * and only fill device-writable descriptors. */
        uint32_t written = 0;
        uint32_t n = 0, idx = hd;
        while (written < flen && n < q_num) {
            uint64_t d    = v->q_desc[NET_QUEUE_RX] + (uint64_t)idx * 16;
            uint64_t gpa  = phys_read(m, d + 0, 8);
            uint32_t dlen = (uint32_t)phys_read(m, d + 8, 4);
            uint16_t df   = (uint16_t)phys_read(m, d + 12, 2);
            uint16_t next = (uint16_t)phys_read(m, d + 14, 2);
            n++;
            if (df & VIRTQ_DESC_F_WRITE) {
                uint32_t chunk = flen - written;
                if (chunk > dlen) chunk = dlen;
                phys_write_blk(m, gpa, frame + written, chunk);
                written += chunk;
            }
            if (!(df & VIRTQ_DESC_F_NEXT)) break;
            idx = next;
        }

        push_used(v, NET_QUEUE_RX, hd, written);
        did = true;
    }

    if (did) {
        v->isr |= 1;
        gic_set_irq(v->gic, v->irq, 1);
    }
}

/* ---- TX: drain guest TX queue into slirp ---- */

static void net_tx_process(VirtIONet *v) {
    Machine *m = v->m;
    if (!v->q_ready[NET_QUEUE_TX] || v->q_num[NET_QUEUE_TX] == 0) return;

    uint16_t avail_idx = (uint16_t)phys_read(m, v->q_avail[NET_QUEUE_TX] + 2, 2);
    bool did = false;

    while (v->last_avail[NET_QUEUE_TX] != avail_idx) {
        uint16_t q_num = (uint16_t)v->q_num[NET_QUEUE_TX];
        uint16_t hd = (uint16_t)phys_read(m,
            v->q_avail[NET_QUEUE_TX] + 4 +
            (uint64_t)(v->last_avail[NET_QUEUE_TX] % q_num) * 2, 2);
        v->last_avail[NET_QUEUE_TX]++;

        /* Gather descriptor chain into a flat buffer. */
        uint8_t  pkt[NET_HDR_LEN + MAX_ETH_FRAME];
        uint32_t plen = 0;
        uint32_t n = 0, idx = hd;
        for (;;) {
            if (n >= q_num) break;
            uint64_t d    = v->q_desc[NET_QUEUE_TX] + (uint64_t)idx * 16;
            uint64_t gpa  = phys_read(m, d + 0, 8);
            uint32_t dlen = (uint32_t)phys_read(m, d + 8, 4);
            uint16_t df   = (uint16_t)phys_read(m, d + 12, 2);
            uint16_t next = (uint16_t)phys_read(m, d + 14, 2);
            uint32_t chunk = dlen;
            if (plen + chunk > sizeof(pkt)) chunk = (uint32_t)(sizeof(pkt) - plen);
            phys_read_blk(m, gpa, pkt + plen, chunk);
            plen += chunk;
            n++;
            if (!(df & VIRTQ_DESC_F_NEXT)) break;
            idx = next;
        }

        /* Skip the virtio_net_hdr; pass the raw Ethernet frame to the backend. */
        if (plen > NET_HDR_LEN) {
            if (v->un) usernet_input(v->un, pkt + NET_HDR_LEN, plen - NET_HDR_LEN);
            else       slirp_input(v->slirp, pkt + NET_HDR_LEN, (int)(plen - NET_HDR_LEN));
        }

        push_used(v, NET_QUEUE_TX, hd, 0);
        did = true;
    }

    if (did) {
        v->isr |= 1;
        gic_set_irq(v->gic, v->irq, 1);
    }
}

/* ---- backend RX entry: queue a frame for delivery to the guest ---- */

/* Returns false when the software RX ring is full: usernet uses that as
 * backpressure (TCP data is simply not emitted yet); the slirp wrapper has
 * to lie and claim success, leaving recovery to TCP retransmission. */
static bool net_deliver_frame(void *opaque, const uint8_t *buf, size_t len) {
    VirtIONet *v = opaque;
    if (len > MAX_ETH_FRAME) return true;              /* oversized: drop */
    int next_tail = (v->rx_tail + 1) & (RX_RING_SIZE - 1);
    if (next_tail == v->rx_head) return false;         /* ring full */
    int slot = v->rx_tail;
    v->rx_tail = next_tail;

    memset(v->rx_buf[slot], 0, NET_HDR_LEN);          /* zero virtio_net_hdr */
    v->rx_buf[slot][10] = 1;                           /* num_buffers = 1 (LE) */
    memcpy(v->rx_buf[slot] + NET_HDR_LEN, buf, len);
    v->rx_len[slot] = NET_HDR_LEN + (uint32_t)len;
    return true;
}

/* ---- slirp callbacks ---- */

static slirp_ssize_t slirp_send_pkt(const void *buf, size_t len, void *opaque) {
    net_deliver_frame(opaque, buf, len);
    return (slirp_ssize_t)len;
}

static void slirp_guest_error(const char *msg, void *opaque) {
    (void)opaque;
    fprintf(stderr, "[virtio-net/slirp] %s\n", msg);
}

static int64_t slirp_clock_ns(void *opaque) {
    (void)opaque;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void *slirp_timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque) {
    VirtIONet *v = opaque;
    NetTimer *t = calloc(1, sizeof(NetTimer));
    t->cb = cb; t->cb_opaque = cb_opaque;
    t->next = v->timers; v->timers = t;
    return t;
}

static void slirp_timer_free(void *timer, void *opaque) {
    VirtIONet *v = opaque;
    NetTimer *t = timer;
    NetTimer **p = &v->timers;
    while (*p && *p != t) p = &(*p)->next;
    if (*p) *p = t->next;
    free(t);
}

static void slirp_timer_mod(void *timer, int64_t expire_ms, void *opaque) {
    (void)opaque;
    ((NetTimer *)timer)->expire_ms = expire_ms;
}

static void slirp_noop_register(slirp_os_socket fd, void *opaque) {
    (void)fd; (void)opaque;
}

static void slirp_noop_notify(void *opaque) { (void)opaque; }

/* ---- poll context for virtio_net_poll ---- */

typedef struct {
    struct pollfd pfds[NET_POLL_MAX];
    int nfds;
} PollCtx;

static int add_poll_socket(slirp_os_socket fd, int events, void *opaque) {
    PollCtx *ctx = opaque;
    if (ctx->nfds >= NET_POLL_MAX) return -1;
    int idx = ctx->nfds++;
    ctx->pfds[idx].fd = (int)fd;
    ctx->pfds[idx].events = 0;
    if (events & SLIRP_POLL_IN)  ctx->pfds[idx].events |= POLLIN;
    if (events & SLIRP_POLL_OUT) ctx->pfds[idx].events |= POLLOUT;
    if (events & SLIRP_POLL_PRI) ctx->pfds[idx].events |= POLLPRI;
    ctx->pfds[idx].revents = 0;
    return idx;
}

static int get_revents(int idx, void *opaque) {
    PollCtx *ctx = opaque;
    if (idx < 0 || idx >= ctx->nfds) return 0;
    int rev = ctx->pfds[idx].revents;
    int r = 0;
    if (rev & POLLIN)  r |= SLIRP_POLL_IN;
    if (rev & POLLOUT) r |= SLIRP_POLL_OUT;
    if (rev & POLLPRI) r |= SLIRP_POLL_PRI;
    if (rev & POLLERR) r |= SLIRP_POLL_ERR;
    if (rev & POLLHUP) r |= SLIRP_POLL_HUP;
    return r;
}

static void net_fire_timers(VirtIONet *v) {
    int64_t now_ms = slirp_clock_ns(v) / 1000000LL;
    for (NetTimer *t = v->timers; t; t = t->next) {
        if (t->expire_ms && t->expire_ms <= now_ms) {
            t->expire_ms = 0;
            t->cb(t->cb_opaque);
        }
    }
}

/* Called from machine_tick() every ~1024 guest instructions. */
void virtio_net_poll(VirtIONet *v) {
    if (v->un) {
        usernet_poll(v->un);
        net_flush_rx(v);
        return;
    }
    net_fire_timers(v);
    PollCtx ctx = {0};
    uint32_t timeout = 0;
    slirp_pollfds_fill_socket(v->slirp, &timeout, add_poll_socket, &ctx);
    if (ctx.nfds > 0) poll(ctx.pfds, ctx.nfds, 0);
    slirp_pollfds_poll(v->slirp, 0, get_revents, &ctx);
    net_flush_rx(v);
}

/* ---- MMIO ---- */

static u64 net_config_read(VirtIONet *v, u64 off, unsigned size) {
    /* struct virtio_net_config: mac[6] @ 0, status[2] @ 6. */
    uint8_t cfg[8];
    memcpy(cfg, v->mac, 6);
    cfg[6] = VIRTIO_NET_S_LINK_UP & 0xff;
    cfg[7] = (VIRTIO_NET_S_LINK_UP >> 8) & 0xff;
    u64 r = 0;
    for (unsigned i = 0; i < size && off + i < sizeof(cfg); i++)
        r |= (u64)cfg[off + i] << (i * 8);
    return r;
}

static u64 net_read(void *opaque, u64 off, unsigned size) {
    VirtIONet *v = opaque;
    if (off >= 0x100) return net_config_read(v, off - 0x100, size);
    int q = (int)v->queue_sel;
    switch (off) {
        case 0x000: return 0x74726976;       /* MagicValue "virt" */
        case 0x004: return 2;                /* Version (modern)  */
        case 0x008: return 1;                /* DeviceID = net    */
        case 0x00c: return 0x554d4551;       /* VendorID "QEMU"   */
        case 0x010:
            return v->dev_feat_sel == 1 ? (u32)(NET_FEATURES >> 32)
                                        : (u32)(NET_FEATURES & 0xffffffff);
        case 0x034: return (q == 0 || q == 1) ? QUEUE_NUM_MAX : 0; /* QueueNumMax */
        case 0x044: return (q == 0 || q == 1) ? v->q_ready[q] : 0; /* QueueReady */
        case 0x060: return v->isr;           /* InterruptStatus   */
        case 0x070: return v->status;        /* Status            */
        case 0x0fc: return 0;                /* ConfigGeneration  */
        default:    return 0;
    }
}

static void net_write(void *opaque, u64 off, unsigned size, u64 val) {
    VirtIONet *v = opaque;
    u32 v32 = (u32)val;
    int q = (int)v->queue_sel;
    switch (off) {
        case 0x014: v->dev_feat_sel = v32; break;
        case 0x020:
            if (v->drv_feat_sel == 1) v->drv_feat = (v->drv_feat & 0xffffffffULL) | ((u64)v32 << 32);
            else                      v->drv_feat = (v->drv_feat & ~0xffffffffULL) | v32;
            break;
        case 0x024: v->drv_feat_sel = v32; break;
        case 0x030: v->queue_sel = v32; break;
        case 0x038:
            if (q == 0 || q == 1)
                v->q_num[q] = v32 > QUEUE_NUM_MAX ? QUEUE_NUM_MAX : v32;
            break;
        case 0x044:
            if (q == 0 || q == 1) v->q_ready[q] = v32;
            break;
        case 0x050:                                         /* QueueNotify: val = queue index */
            if (v32 == NET_QUEUE_TX) net_tx_process(v);
            else if (v32 == NET_QUEUE_RX) net_flush_rx(v); /* guest added free buffers */
            break;
        case 0x064:
            v->isr &= ~v32;
            if (v->isr == 0) gic_set_irq(v->gic, v->irq, 0);
            break;
        case 0x070:
            if (v32 == 0) net_reset(v); else v->status = v32;
            break;
        /* Queue descriptor/available/used table addresses (low/high 32-bit pairs). */
        case 0x080:
            if (q == 0 || q == 1) { v->q_desc[q]  = (v->q_desc[q]  & ~0xffffffffULL) | v32; } break;
        case 0x084:
            if (q == 0 || q == 1) { v->q_desc[q]  = (v->q_desc[q]  &  0xffffffffULL) | ((u64)v32 << 32); } break;
        case 0x090:
            if (q == 0 || q == 1) { v->q_avail[q] = (v->q_avail[q] & ~0xffffffffULL) | v32; } break;
        case 0x094:
            if (q == 0 || q == 1) { v->q_avail[q] = (v->q_avail[q] &  0xffffffffULL) | ((u64)v32 << 32); } break;
        case 0x0a0:
            if (q == 0 || q == 1) { v->q_used[q]  = (v->q_used[q]  & ~0xffffffffULL) | v32; } break;
        case 0x0a4:
            if (q == 0 || q == 1) { v->q_used[q]  = (v->q_used[q]  &  0xffffffffULL) | ((u64)v32 << 32); } break;
        default: break;
    }
}

/* ---- creation ---- */

VirtIONet *virtio_net_create(Machine *m, GIC *gic) {
    VirtIONet *v = calloc(1, sizeof(*v));
    v->m = m; v->gic = gic; v->irq = INTID_VIRTIO0;   /* slot 0 */

    /* Stable MAC: 52:54:00:12:34:56 (QEMU's default range). */
    static const uint8_t default_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    memcpy(v->mac, default_mac, 6);

    /* Transitional backend switch while the in-tree stack is brought up:
     * AENET=user selects usernet, anything else keeps libslirp. */
    const char *backend = getenv("AENET");
    if (backend && !strcmp(backend, "user")) {
        v->un = usernet_new(net_deliver_frame, v, v->mac);
        if (!v->un) { fprintf(stderr, "[virtio-net] usernet_new failed\n"); exit(1); }
        for (int i = 0; i < m->n_net_fwds; i++) {
            int r = usernet_add_hostfwd(v->un, m->net_fwds[i].is_udp,
                                        (uint16_t)m->net_fwds[i].host_port,
                                        (uint16_t)m->net_fwds[i].guest_port);
            fprintf(stderr, "[virtio-net] port forward %s %d -> 10.0.2.15:%d%s\n",
                    m->net_fwds[i].is_udp ? "udp" : "tcp",
                    m->net_fwds[i].host_port, m->net_fwds[i].guest_port,
                    r < 0 ? " FAILED" : "");
        }
        machine_add_device(m, 0x0a000000, 0x200, net_read, net_write, v, "virtio-net");
        m->net = v;
        fprintf(stderr, "[virtio-net] backend usernet, MAC %02x:%02x:%02x:%02x:%02x:%02x, "
                "guest 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3\n",
                v->mac[0], v->mac[1], v->mac[2], v->mac[3], v->mac[4], v->mac[5]);
        return v;
    }

    static const SlirpCb cbs = {
        .send_packet          = slirp_send_pkt,
        .guest_error          = slirp_guest_error,
        .clock_get_ns         = slirp_clock_ns,
        .timer_new            = slirp_timer_new,
        .timer_free           = slirp_timer_free,
        .timer_mod            = slirp_timer_mod,
        .register_poll_fd     = NULL,   /* not called: cfg_version >= 6 */
        .unregister_poll_fd   = NULL,
        .notify               = slirp_noop_notify,
        .init_completed       = NULL,
        .timer_new_opaque     = NULL,   /* falls back to timer_new */
        .register_poll_socket   = slirp_noop_register,
        .unregister_poll_socket = slirp_noop_register,
    };

    SlirpConfig cfg = {0};
    cfg.version        = 6;             /* needed for register_poll_socket path */
    cfg.in_enabled     = true;
    cfg.in6_enabled    = false;
    inet_pton(AF_INET, "10.0.2.0",   &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2",   &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.15",  &cfg.vdhcp_start);
    inet_pton(AF_INET, "10.0.2.3",   &cfg.vnameserver);

    v->slirp = slirp_new(&cfg, &cbs, v);
    if (!v->slirp) { fprintf(stderr, "[virtio-net] slirp_new failed\n"); exit(1); }

    /* Apply port-forwarding rules from -netfwd options. */
    struct in_addr host_addr = {INADDR_ANY};
    struct in_addr guest_addr;
    inet_pton(AF_INET, "10.0.2.15", &guest_addr);
    for (int i = 0; i < m->n_net_fwds; i++) {
        if (slirp_add_hostfwd(v->slirp, m->net_fwds[i].is_udp,
                              host_addr, m->net_fwds[i].host_port,
                              guest_addr, m->net_fwds[i].guest_port) < 0) {
            fprintf(stderr, "[virtio-net] port forward %s:%d -> 10.0.2.15:%d failed\n",
                    m->net_fwds[i].is_udp ? "udp" : "tcp",
                    m->net_fwds[i].host_port, m->net_fwds[i].guest_port);
        } else {
            fprintf(stderr, "[virtio-net] port forward %s %d -> 10.0.2.15:%d\n",
                    m->net_fwds[i].is_udp ? "udp" : "tcp",
                    m->net_fwds[i].host_port, m->net_fwds[i].guest_port);
        }
    }

    machine_add_device(m, 0x0a000000, 0x200, net_read, net_write, v, "virtio-net");
    m->net = v;
    fprintf(stderr, "[virtio-net] MAC %02x:%02x:%02x:%02x:%02x:%02x, "
            "guest 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3\n",
            v->mac[0], v->mac[1], v->mac[2], v->mac[3], v->mac[4], v->mac[5]);
    return v;
}
