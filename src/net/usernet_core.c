/* usernet core: API entry points, Ethernet/ARP/IPv4 handling, ICMP echo,
 * frame emission, poll driver, pcap capture (AENET_PCAP=<file>) and stats
 * (AENET_STATS=1). Protocol proxies live in usernet_udp.c / usernet_tcp.c. */
#include "usernet_priv.h"
#include <stdlib.h>
#include <time.h>

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
    if (l4len > UN_MTU - 20 || !un->guest_mac_known) return false;
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

    /* learn the guest MAC from any ARP it sends */
    memcpy(un->guest_mac, pkt + 8, 6);
    un->guest_mac_known = true;

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

/* ---- ICMP (echo to the virtual hosts; external echo arrives later) ---- */

static void icmp_input(UserNet *un, uint32_t src, uint32_t dst,
                       const uint8_t *icmp, size_t len) {
    if (len < 8 || icmp[0] != 8 || icmp[1] != 0) return;   /* echo request */
    if (dst != UN_IP_HOST && dst != UN_IP_DNS) return;

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
        case UN_IPP_UDP:  break;         /* usernet_udp.c, next commits */
        case UN_IPP_TCP:  break;         /* usernet_tcp.c, next commits */
        default: break;
    }
}

/* ---- public API ---- */

void usernet_input(UserNet *un, const uint8_t *frame, size_t len) {
    if (len < 14) return;
    un->st_in_frames++;
    un->now_ms = un_now_ms();
    pcap_frame(un, frame, len);

    /* learn the guest MAC from the Ethernet source */
    if (!un->guest_mac_known || memcmp(un->guest_mac, frame + 6, 6) != 0) {
        memcpy(un->guest_mac, frame + 6, 6);
        un->guest_mac_known = true;
    }

    switch (un_rd16(frame + 12)) {
        case UN_ETH_ARP: arp_input(un, frame + 14, len - 14); break;
        case UN_ETH_IP:  ip_input(un, frame + 14, len - 14);  break;
        default: break;                  /* IPv6 etc.: not supported */
    }
}

void usernet_poll(UserNet *un) {
    un->now_ms = un_now_ms();
    un->npfds = 0;
    /* Protocol handlers register their fds here as they land in later
     * commits; until then there is nothing to poll. */
    if (un->npfds > 0)
        poll(un->pfds, (nfds_t)un->npfds, 0);
}

int usernet_add_hostfwd(UserNet *un, bool is_udp,
                        uint16_t host_port, uint16_t guest_port) {
    (void)un; (void)is_udp; (void)host_port; (void)guest_port;
    fprintf(stderr, "[usernet] hostfwd not implemented yet\n");
    return -1;
}

void usernet_guest_reset(UserNet *un) {
    (void)un;                            /* no flows to tear down yet */
}

static void un_stats_dump(void) {
    UserNet *un = un_stats_instance;
    if (!un) return;
    fprintf(stderr, "[usernet] stats: in=%llu out=%llu refused=%llu drop=%llu\n",
            (unsigned long long)un->st_in_frames,
            (unsigned long long)un->st_out_ok,
            (unsigned long long)un->st_out_refused,
            (unsigned long long)un->st_out_drop);
}

UserNet *usernet_new(usernet_output_fn output, void *opaque) {
    UserNet *un = calloc(1, sizeof(*un));
    if (!un) return NULL;
    un->output = output;
    un->opaque = opaque;
    un->now_ms = un_now_ms();
    pcap_open(un);
    if (getenv("AENET_STATS")) {
        un_stats_instance = un;
        atexit(un_stats_dump);
    }
    return un;
}
