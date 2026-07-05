/* usernet internals shared between usernet_core.c and the per-protocol files.
 * Addresses and ports are kept in HOST byte order everywhere internally; the
 * un_rd/un_wr helpers convert at the wire boundary. */
#ifndef USERNET_PRIV_H
#define USERNET_PRIV_H

#include "usernet.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>

/* ---- fixed topology (host byte order) ---- */
#define UN_IP(a,b,c,d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                        ((uint32_t)(c) << 8) | (uint32_t)(d))
#define UN_IP_HOST   UN_IP(10,0,2,2)     /* gateway; also reaches host loopback */
#define UN_IP_DNS    UN_IP(10,0,2,3)     /* virtual DNS, redirected to real one */
#define UN_IP_GUEST  UN_IP(10,0,2,15)
#define UN_NET       UN_IP(10,0,2,0)
#define UN_NETMASK   0xffffff00u

#define UN_MTU       1500
#define UN_MAX_FRAME (14 + UN_MTU)       /* eth header + MTU, no FCS */
#define UN_POLL_MAX  128

#define UN_ETH_IP    0x0800
#define UN_ETH_ARP   0x0806
#define UN_IPP_ICMP  1
#define UN_IPP_TCP   6
#define UN_IPP_UDP   17

extern const uint8_t un_host_mac[6];     /* 52:55:0a:00:02:02 */

/* ---- poll table entry kinds ---- */
enum { UN_PK_NONE = 0 };

struct UserNet {
    usernet_output_fn output;
    void   *opaque;

    uint8_t guest_mac[6];
    bool    guest_mac_known;

    int64_t  now_ms;                     /* refreshed at each poll/input */
    uint16_t ip_id;                      /* incrementing, deterministic */

    /* poll table, rebuilt from live flows on every usernet_poll() */
    struct pollfd pfds[UN_POLL_MAX];
    struct { uint8_t kind; void *obj; } pref[UN_POLL_MAX];
    int npfds;

    /* stats (printed at exit with AENET_STATS=1) */
    uint64_t st_in_frames, st_out_ok, st_out_refused, st_out_drop;

    FILE *pcap;                          /* AENET_PCAP=<file>, else NULL */
};

/* ---- big-endian field access ---- */
static inline uint16_t un_rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t un_rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static inline void un_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void un_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* ---- internet checksum ---- */
static inline uint32_t un_cksum_add(uint32_t sum, const uint8_t *p, size_t n) {
    while (n > 1) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; n -= 2; }
    if (n) sum += (uint32_t)(p[0] << 8);
    return sum;
}
static inline uint16_t un_cksum_fin(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)(~sum & 0xffff);
}
/* TCP/UDP checksum incl. IPv4 pseudo-header; l4 has the checksum field zeroed. */
uint16_t un_l4_cksum(uint32_t src, uint32_t dst, uint8_t proto,
                     const uint8_t *l4, size_t len);

int64_t un_now_ms(void);

/* Frame output via the device callback (pcap + stats accounted).
 * false = device RX ring full, frame not delivered. */
bool un_output(UserNet *un, const uint8_t *frame, size_t len);

/* Build eth+IPv4 around an L4 payload and emit it to the guest.
 * src/dst in host byte order; l4len <= UN_MTU - 20. */
bool un_emit_ip(UserNet *un, uint8_t proto, uint32_t src, uint32_t dst,
                const uint8_t *l4, size_t l4len);

#endif
