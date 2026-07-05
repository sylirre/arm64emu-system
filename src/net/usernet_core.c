/* usernet core: API entry points, Ethernet/ARP/IPv4 handling, ICMP echo,
 * frame emission, poll driver, pcap capture (AENET_PCAP=<file>) and stats
 * (AENET_STATS=1). Protocol proxies live in usernet_udp.c / usernet_tcp.c. */
#include "usernet_priv.h"
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

const uint8_t un_host_mac[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static UserNet *un_stats_instance;      /* for the atexit stats dump */

int64_t un_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint16_t un_l4_cksum(uint32_t src, uint32_t dst, uint8_t proto,
                     const uint8_t *l4, size_t len) {
    uint8_t ph[12];
    un_wr32(ph + 0, src);
    un_wr32(ph + 4, dst);
    ph[8] = 0; ph[9] = proto;
    un_wr16(ph + 10, (uint16_t)len);
    return un_cksum_fin(un_cksum_add(un_cksum_add(0, ph, 12), l4, len));
}

/* ---- pcap capture ---- */

static void pcap_frame(UserNet *un, const uint8_t *frame, size_t len) {
    if (!un->pcap) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t rec[4] = { (uint32_t)ts.tv_sec, (uint32_t)(ts.tv_nsec / 1000),
                        (uint32_t)len, (uint32_t)len };
    fwrite(rec, sizeof(rec), 1, un->pcap);
    fwrite(frame, len, 1, un->pcap);
    fflush(un->pcap);
}

static void pcap_open(UserNet *un) {
    const char *path = getenv("AENET_PCAP");
    if (!path || !*path) return;
    un->pcap = fopen(path, "wb");
    if (!un->pcap) { perror("[usernet] AENET_PCAP fopen"); return; }
    /* layout: magic, major|minor<<16, thiszone, sigfigs, snaplen, network */
    uint32_t hdr[6] = { 0xa1b2c3d4, 2 | (4 << 16), 0, 0, 65535, 1 };
    fwrite(hdr, sizeof(hdr), 1, un->pcap);
}

/* ---- frame output ---- */

bool un_output(UserNet *un, const uint8_t *frame, size_t len) {
    if (!un->output(un->opaque, frame, len)) {
        un->st_out_refused++;
        return false;
    }
    un->st_out_ok++;
    pcap_frame(un, frame, len);
    return true;
}

bool un_emit_ip(UserNet *un, uint8_t proto, uint32_t src, uint32_t dst,
                const uint8_t *l4, size_t l4len) {
    if (l4len > UN_MTU - 20) return false;
    uint8_t buf[UN_MAX_FRAME];

    memcpy(buf + 0, un->guest_mac, 6);
    memcpy(buf + 6, un_host_mac, 6);
    un_wr16(buf + 12, UN_ETH_IP);

    uint8_t *ip = buf + 14;
    ip[0] = 0x45; ip[1] = 0;
    un_wr16(ip + 2, (uint16_t)(20 + l4len));
    un_wr16(ip + 4, un->ip_id++);
    un_wr16(ip + 6, 0);                  /* no flags, no fragmentation */
    ip[8] = 64; ip[9] = proto;
    un_wr16(ip + 10, 0);
    un_wr32(ip + 12, src);
    un_wr32(ip + 16, dst);
    un_wr16(ip + 10, un_cksum_fin(un_cksum_add(0, ip, 20)));

    memcpy(ip + 20, l4, l4len);
    return un_output(un, buf, 14 + 20 + l4len);
}

/* ---- ARP ---- */

static void arp_input(UserNet *un, const uint8_t *pkt, size_t len) {
    if (len < 28) return;
    if (un_rd16(pkt) != 1 || un_rd16(pkt + 2) != UN_ETH_IP) return;
    if (pkt[4] != 6 || pkt[5] != 4) return;
    uint16_t oper = un_rd16(pkt + 6);
    uint32_t spa  = un_rd32(pkt + 14);
    uint32_t tpa  = un_rd32(pkt + 24);

    /* track the guest MAC from any ARP it sends */
    memcpy(un->guest_mac, pkt + 8, 6);

    if (oper != 1) return;               /* only requests need answering */
    /* Proxy-answer for every on-subnet address except the guest's own
     * (so DAD probes for 10.0.2.15 see no conflict). */
    if ((tpa & UN_NETMASK) != UN_NET || tpa == UN_IP_GUEST) return;

    uint8_t rep[42];
    memcpy(rep + 0, pkt + 8, 6);         /* back to the requester */
    memcpy(rep + 6, un_host_mac, 6);
    un_wr16(rep + 12, UN_ETH_ARP);
    uint8_t *a = rep + 14;
    un_wr16(a + 0, 1); un_wr16(a + 2, UN_ETH_IP);
    a[4] = 6; a[5] = 4;
    un_wr16(a + 6, 2);                   /* reply */
    memcpy(a + 8, un_host_mac, 6);
    un_wr32(a + 14, tpa);
    memcpy(a + 18, pkt + 8, 6);
    un_wr32(a + 24, spa);
    un_output(un, rep, sizeof(rep));
}

/* ---- ICMP: local echo for the virtual hosts, relayed echo elsewhere ---- */

/* External echo goes through unprivileged "ping sockets" (SOCK_DGRAM,
 * IPPROTO_ICMP). Availability depends on net.ipv4.ping_group_range; if the
 * host denies them, external pings are silently dropped (one stderr note). */
static void icmp_relay(UserNet *un, uint32_t dst,
                       const uint8_t *icmp, size_t len) {
    if (un->ping_unavail) return;
    uint16_t g_id = un_rd16(icmp + 4);

    IcmpFlow *f = NULL, *spare = NULL, *oldest = &un->icmp[0];
    for (int i = 0; i < UN_ICMP_MAX; i++) {
        IcmpFlow *e = &un->icmp[i];
        if (e->in_use && e->g_id == g_id && e->dst_ip == dst) { f = e; break; }
        if (!e->in_use && !spare) spare = e;
        if (e->last_ms < oldest->last_ms) oldest = e;
    }
    if (!f) {
        f = spare ? spare : oldest;
        if (f->in_use) { close(f->fd); f->in_use = false; }
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        IPPROTO_ICMP);
        if (fd < 0) {
            if (errno == EACCES || errno == EPERM || errno == EAFNOSUPPORT ||
                errno == EPROTONOSUPPORT) {
                un->ping_unavail = true;
                fprintf(stderr, "[usernet] ping sockets unavailable "
                        "(net.ipv4.ping_group_range?); guest pings to "
                        "external hosts are dropped\n");
            }
            return;
        }
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(dst);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            close(fd);
            return;
        }
        f->in_use = true;
        f->g_id = g_id;
        f->dst_ip = dst;
        f->fd = fd;
    }
    f->last_ms = un->now_ms;
    /* The kernel replaces the id and refreshes the checksum on the way out. */
    send(f->fd, icmp, len, MSG_NOSIGNAL);
}

static void icmp_readable(UserNet *un, IcmpFlow *f) {
    uint8_t buf[UN_MTU - 20];
    for (int burst = 0; burst < 8; burst++) {
        ssize_t n = recv(f->fd, buf, sizeof(buf), 0);
        if (n < 8) return;
        if (buf[0] != 0 || buf[1] != 0) continue;         /* echo reply only */
        f->last_ms = un->now_ms;
        un_wr16(buf + 4, f->g_id);                        /* restore guest id */
        un_wr16(buf + 2, 0);
        un_wr16(buf + 2, un_cksum_fin(un_cksum_add(0, buf, (size_t)n)));
        if (!un_emit_ip(un, UN_IPP_ICMP, f->dst_ip, UN_IP_GUEST,
                        buf, (size_t)n))
            return;
    }
}

static void icmp_fill(UserNet *un) {
    for (int i = 0; i < UN_ICMP_MAX; i++)
        if (un->icmp[i].in_use)
            un_poll_add(un, un->icmp[i].fd, POLLIN, UN_PK_ICMP, &un->icmp[i]);
}

static void icmp_tick(UserNet *un) {
    if (un->now_ms - un->icmp_sweep_ms < 1000) return;
    un->icmp_sweep_ms = un->now_ms;
    for (int i = 0; i < UN_ICMP_MAX; i++) {
        IcmpFlow *f = &un->icmp[i];
        if (f->in_use && un->now_ms - f->last_ms > UN_ICMP_TTL_MS) {
            close(f->fd);
            f->in_use = false;
        }
    }
}

static void icmp_reset(UserNet *un) {
    for (int i = 0; i < UN_ICMP_MAX; i++) {
        if (un->icmp[i].in_use) {
            close(un->icmp[i].fd);
            un->icmp[i].in_use = false;
        }
    }
}

static void icmp_input(UserNet *un, uint32_t src, uint32_t dst,
                       const uint8_t *icmp, size_t len) {
    if (len < 8 || icmp[0] != 8 || icmp[1] != 0) return;   /* echo request */

    if (dst != UN_IP_HOST && dst != UN_IP_DNS) {
        if ((dst & UN_NETMASK) != UN_NET)
            icmp_relay(un, dst, icmp, len);
        return;
    }

    uint8_t rep[UN_MTU - 20];
    if (len > sizeof(rep)) return;
    memcpy(rep, icmp, len);
    rep[0] = 0;                          /* echo reply */
    un_wr16(rep + 2, 0);
    un_wr16(rep + 2, un_cksum_fin(un_cksum_add(0, rep, len)));
    un_emit_ip(un, UN_IPP_ICMP, dst, src, rep, len);
}

/* ---- IPv4 demux ---- */

static void ip_input(UserNet *un, const uint8_t *ip, size_t len) {
    if (len < 20 || (ip[0] >> 4) != 4) return;
    size_t ihl = (size_t)(ip[0] & 0xf) * 4;
    size_t tot = un_rd16(ip + 2);
    if (ihl < 20 || tot < ihl || tot > len) return;
    if (un_cksum_fin(un_cksum_add(0, ip, ihl)) != 0) return;
    if (un_rd16(ip + 6) & 0x3fff) return;   /* fragments unsupported */

    uint32_t src = un_rd32(ip + 12), dst = un_rd32(ip + 16);
    const uint8_t *l4 = ip + ihl;
    size_t l4len = tot - ihl;

    switch (ip[9]) {
        case UN_IPP_ICMP: icmp_input(un, src, dst, l4, l4len); break;
        case UN_IPP_UDP:  un_udp_input(un, src, dst, l4, l4len); break;
        case UN_IPP_TCP:  un_tcp_input(un, src, dst, l4, l4len); break;
        default: break;
    }
}

/* ---- public API ---- */

void usernet_input(UserNet *un, const uint8_t *frame, size_t len) {
    if (len < 14) return;
    un->st_in_frames++;
    un->now_ms = un_now_ms();
    pcap_frame(un, frame, len);

    /* track the guest MAC in case the guest overrides the advertised one */
    if (memcmp(un->guest_mac, frame + 6, 6) != 0)
        memcpy(un->guest_mac, frame + 6, 6);

    switch (un_rd16(frame + 12)) {
        case UN_ETH_ARP: arp_input(un, frame + 14, len - 14); break;
        case UN_ETH_IP:  ip_input(un, frame + 14, len - 14);  break;
        default: break;                  /* IPv6 etc.: not supported */
    }
}

void usernet_poll(UserNet *un) {
    un->now_ms = un_now_ms();
    un_udp_tick(un);
    un_tcp_tick(un);
    icmp_tick(un);

    un->npfds = 0;
    un_udp_fill(un);
    un_tcp_fill(un);
    icmp_fill(un);
    for (int i = 0; i < un->n_fwd; i++)
        un_poll_add(un, un->fwd[i].fd, POLLIN,
                    un->fwd[i].is_udp ? UN_PK_UDP_FWD : UN_PK_TCP_LISTEN,
                    &un->fwd[i]);
    if (un->npfds > 0 && poll(un->pfds, (nfds_t)un->npfds, 0) > 0) {
        for (int i = 0; i < un->npfds; i++) {
            if (!un->pfds[i].revents) continue;
            switch (un->pref[i].kind) {
                case UN_PK_UDP:  un_udp_readable(un, un->pref[i].obj); break;
                case UN_PK_TCP:  un_tcp_event(un, un->pref[i].obj,
                                              un->pfds[i].revents); break;
                case UN_PK_ICMP: icmp_readable(un, un->pref[i].obj); break;
                case UN_PK_TCP_LISTEN:
                    un_tcp_accept(un, un->pref[i].obj); break;
                case UN_PK_UDP_FWD:
                    un_udp_fwd_readable(un, un->pref[i].obj); break;
                default: break;
            }
        }
    }
    /* push buffered host->guest TCP data even when poll() saw nothing:
     * the guest may have opened its window or freed RX-ring slots */
    un_tcp_emit_all(un);
}

int usernet_add_hostfwd(UserNet *un, bool is_udp,
                        uint16_t host_port, uint16_t guest_port) {
    if (un->n_fwd >= UN_FWD_MAX) return -1;

    int fd = socket(AF_INET,
                    (is_udp ? SOCK_DGRAM : SOCK_STREAM) |
                    SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(host_port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        (!is_udp && listen(fd, 8) < 0)) {
        close(fd);
        return -1;
    }

    HostFwd *fw = &un->fwd[un->n_fwd++];
    fw->is_udp = is_udp;
    fw->host_port = host_port;
    fw->guest_port = guest_port;
    fw->fd = fd;
    return 0;
}

void usernet_guest_reset(UserNet *un) {
    un_udp_reset(un);
    un_tcp_reset(un);
    icmp_reset(un);
}

uint32_t un_dns_server(UserNet *un) {
    if (un->dns_ip && un->now_ms - un->dns_check_ms < 5000)
        return un->dns_ip;
    un->dns_check_ms = un->now_ms;

    uint32_t ip = 0;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (f) {
        char line[256];
        while (!ip && fgets(line, sizeof(line), f)) {
            unsigned a, b, c, d;
            if (sscanf(line, " nameserver %u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
                a < 256 && b < 256 && c < 256 && d < 256)
                ip = UN_IP(a, b, c, d);
        }
        fclose(f);
    }
    if (!ip) {
        ip = UN_IP(127,0,0,53);          /* systemd-resolved stub, last resort */
        if (!un->dns_warned) {
            un->dns_warned = true;
            fprintf(stderr, "[usernet] no IPv4 nameserver in /etc/resolv.conf, "
                    "trying 127.0.0.53\n");
        }
    }
    un->dns_ip = ip;
    return ip;
}

static void un_stats_dump(void) {
    UserNet *un = un_stats_instance;
    if (!un) return;
    fprintf(stderr, "[usernet] stats: in=%llu out=%llu refused=%llu drop=%llu "
            "tcp_conns=%llu tcp_rtx=%llu\n",
            (unsigned long long)un->st_in_frames,
            (unsigned long long)un->st_out_ok,
            (unsigned long long)un->st_out_refused,
            (unsigned long long)un->st_out_drop,
            (unsigned long long)un->st_tcp_conns,
            (unsigned long long)un->st_tcp_rtx);
}

UserNet *usernet_new(usernet_output_fn output, void *opaque,
                     const uint8_t guest_mac[6]) {
    UserNet *un = calloc(1, sizeof(*un));
    if (!un) return NULL;
    un->output = output;
    un->opaque = opaque;
    memcpy(un->guest_mac, guest_mac, 6);
    un->now_ms = un_now_ms();
    pcap_open(un);
    if (getenv("AENET_STATS")) {
        un_stats_instance = un;
        atexit(un_stats_dump);
    }
    return un;
}
