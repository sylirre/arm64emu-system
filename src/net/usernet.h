/* usernet: self-contained user-mode NAT stack for the virtio-net backend.
 *
 * Replaces libslirp with the same fixed topology: guest 10.0.2.15/24,
 * gateway/host 10.0.2.2, DNS 10.0.2.3. Guest TCP/UDP flows are terminated
 * here and proxied through ordinary nonblocking host sockets — no TAP, TUN,
 * or privileges required. IPv4 only.
 *
 * The device layer feeds guest TX frames in via usernet_input() and drives
 * the stack with usernet_poll() (nonblocking, called from machine_tick).
 * Frames for the guest come back through the output callback; returning
 * false means "can't take it now" (device RX ring full) — the stack holds
 * TCP data back (its own backpressure) and drops loss-tolerant frames. */
#ifndef USERNET_H
#define USERNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct UserNet UserNet;

typedef bool (*usernet_output_fn)(void *opaque, const uint8_t *frame, size_t len);

UserNet *usernet_new(usernet_output_fn output, void *opaque);
int      usernet_add_hostfwd(UserNet *un, bool is_udp,
                             uint16_t host_port, uint16_t guest_port);
void     usernet_input(UserNet *un, const uint8_t *frame, size_t len);
void     usernet_poll(UserNet *un);
/* Guest-driven device reset (warm reboot): tear down all flows but keep
 * host-side listeners and sockets that outlive the guest. */
void     usernet_guest_reset(UserNet *un);

#endif
