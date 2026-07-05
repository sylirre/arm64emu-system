#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Sylirre
# End-to-end network smoke test for the usernet backend (-net).
#
# Boots a BusyBox test guest (see tests/net/) against a host-local HTTP
# server, so everything except the optional DNS lookup is hermetic:
#   phase "full"      dhcp+renew, ping gw/dns, DNS, wget, 3MB wget with md5,
#                     10 parallel fetches, half-close, refused-port RST,
#                     200-query UDP flood (flow expiry)
#   phase "upload"    guest pushes 2MB to a host nc listener (md5-checked)
#   phase "nc_listen" -netfwd tcp: five host connections into the guest
#   phase "udp_echo"  -netfwd udp: datagram in, reply out the rule socket
#
# Requirements (see tests/net/mk_guest_initrd.sh):
#   AE_KERNEL      aarch64 EFI-stub kernel   (default: ~/Image.gz)
#   AE_SRC_INITRD  Alpine initramfs to harvest the guest userland from
#   AE_BIOS        firmware                  (default: QEMU_EFI.fd path)
set -u
cd "$(dirname "$0")/../.."

EMU=./arm64emu
BIOS=${AE_BIOS:-/usr/share/qemu-efi-aarch64/QEMU_EFI.fd}
KERNEL=${AE_KERNEL:-$HOME/Image.gz}
IRD=build/net-test.ird
HTTP_PORT=${HTTP_PORT:-8099}
UP_PORT=${UP_PORT:-8098}
FWD_TCP=${FWD_TCP:-8088}
FWD_UDP=${FWD_UDP:-8087}
WWW=$(mktemp -d)
FAIL=0

note() { printf '\n== %s ==\n' "$*"; }
need() { command -v "$1" >/dev/null || { echo "missing tool: $1"; exit 1; }; }
need python3; need nc; need cpio; need md5sum

[ -x "$EMU" ] || { echo "build arm64emu first"; exit 1; }
tests/net/mk_guest_initrd.sh "$IRD" || exit 1

echo "hello-arm64emu-net" > "$WWW/hello.txt"
head -c 3000000 /dev/urandom > "$WWW/big.bin"
BIG_MD5=$(md5sum "$WWW/big.bin" | cut -d' ' -f1)
python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 --directory "$WWW" \
    >/dev/null 2>&1 &
HTTPD=$!
trap 'kill $HTTPD 2>/dev/null; rm -rf "$WWW"' EXIT
sleep 1
kill -0 $HTTPD 2>/dev/null || { echo "http server failed (port $HTTP_PORT busy?)"; exit 1; }
curl -sf "http://127.0.0.1:$HTTP_PORT/hello.txt" >/dev/null 2>&1 || \
    { echo "http server not serving on $HTTP_PORT"; exit 1; }

boot() {  # boot PHASE AEARG [extra emulator args...] -> log on stdout
    local phase=$1 aearg=$2; shift 2
    timeout 420 "$EMU" -bios "$BIOS" -kernel "$KERNEL" -initrd "$IRD" \
        -net -append "aetest=$phase aearg=$aearg" "$@" </dev/null 2>&1
}

expect() {  # expect LOG MARKER...
    local log=$1; shift
    for m in "$@"; do
        if grep -aq "$m" "$log"; then echo "ok   $m"
        else echo "MISS $m"; FAIL=1; fi
    done
}

note "phase full"
LOG=$(mktemp)
boot full "$HTTP_PORT" >"$LOG"
expect "$LOG" \
    "AETEST:dhcp:PASS" "AETEST:dhcp2:PASS" "AETEST:ping_gw:PASS" \
    "AETEST:ping_dns:PASS" "AETEST:wget_host:PASS" "MD5:$BIG_MD5" \
    "AETEST:wget_big:PASS" "AETEST:par:PASS" "AETEST:halfclose:PASS" \
    "AETEST:postclose:PASS" "AETEST:refused:PASS" "AETEST:udp_flood:PASS" \
    "AETEST:DONE"
grep -a "AETEST:dns:" "$LOG"   # informational: needs a working host resolver
rm -f "$LOG"

note "phase upload"
LOG=$(mktemp); UPF=$(mktemp)
( timeout 120 nc -l 127.0.0.1 "$UP_PORT" >"$UPF" ) &
NCPID=$!
boot upload "$UP_PORT" >"$LOG"
wait $NCPID 2>/dev/null
UPMD5=$(grep -ao "UPMD5:[0-9a-f]*" "$LOG" | cut -d: -f2)
if [ -n "$UPMD5" ] && [ "$(md5sum "$UPF" | cut -d' ' -f1)" = "$UPMD5" ]; then
    echo "ok   upload md5 matches ($(wc -c < "$UPF") bytes)"
else
    echo "MISS upload md5"; FAIL=1
fi
expect "$LOG" "AETEST:upload:PASS"
rm -f "$LOG" "$UPF"

note "phase hostfwd tcp"
LOG=$(mktemp)
boot nc_listen 8080 -netfwd "tcp:$FWD_TCP:8080" >"$LOG" &
BOOTPID=$!
for i in $(seq 1 240); do
    grep -aq "AETEST:nc_listen:READY" "$LOG" && break; sleep 1
done
GOT=0
for i in 1 2 3 4 5; do
    # the guest listens with sequential nc -l, so there are brief gaps
    # between connections; retry a refused attempt rather than fail
    for try in 1 2 3 4 5; do
        R=$(printf 'msg-%d\n' "$i" | timeout 10 nc -q1 127.0.0.1 "$FWD_TCP")
        [ "$R" = "hello-from-guest" ] && { GOT=$((GOT+1)); break; }
        sleep 1
    done
    sleep 0.5
done
wait $BOOTPID
echo "hostfwd tcp: $GOT/5"
[ "$GOT" = 5 ] || FAIL=1
expect "$LOG" "AETEST:nc_listen:PASS" "RX:msg-5"
rm -f "$LOG"

note "phase hostfwd udp"
LOG=$(mktemp)
boot udp_echo 8081 -netfwd "udp:$FWD_UDP:8081" >"$LOG" &
BOOTPID=$!
for i in $(seq 1 240); do
    grep -aq "AETEST:udp_echo:READY" "$LOG" && break; sleep 1
done
sleep 2
R=
for try in 1 2 3; do   # first datagram may race the guest listener bind
    R=$(printf 'udp-ping\n' | timeout 10 nc -u -w3 127.0.0.1 "$FWD_UDP")
    [ -n "$R" ] && break
    sleep 1
done
wait $BOOTPID
echo "hostfwd udp reply: '$R'"
[ "$R" = "reply:udp-ping" ] || FAIL=1  # guest nc replies to the learned peer
expect "$LOG" "URX:udp-ping"
rm -f "$LOG"

note "result"
if [ "$FAIL" = 0 ]; then echo "NET SMOKE: ALL PASS"; else echo "NET SMOKE: FAILURES"; fi
exit $FAIL
