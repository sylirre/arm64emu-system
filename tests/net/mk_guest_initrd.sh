#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Sylirre
# Build the network-test guest initramfs for tests/scripts/net_smoke.sh.
#
# The guest userland (busybox + musl loader + virtio/net kernel modules) is
# harvested from a stock Alpine aarch64 initramfs matching the test kernel:
#   AE_SRC_INITRD  path to that initramfs   (default: ~/initrd)
#   $1             output path              (default: build/net-test.ird)
#
# Modules pulled: virtio_mmio, failover, net_failover, virtio_net, af_packet
# (af_packet because udhcpc needs AF_PACKET and Alpine builds it modular).
set -eu
SRC=${AE_SRC_INITRD:-$HOME/initrd}
OUT=${1:-build/net-test.ird}
HERE=$(cd "$(dirname "$0")" && pwd)

[ -f "$SRC" ] || { echo "source initramfs '$SRC' not found (set AE_SRC_INITRD)"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/src" "$WORK/root"/{bin,lib,proc,sys,dev,tmp,etc,usr/share/udhcpc}
( cd "$WORK/src" && zcat "$SRC" | cpio -idm --no-absolute-filenames --quiet )

KREL=$(ls "$WORK/src/lib/modules" | head -1)
MODS="$WORK/src/lib/modules/$KREL/kernel"
cp "$WORK/src/bin/busybox"              "$WORK/root/bin/"
cp "$WORK/src/lib/ld-musl-aarch64.so.1" "$WORK/root/lib/"
for m in drivers/virtio/virtio_mmio.ko net/core/failover.ko \
         drivers/net/net_failover.ko drivers/net/virtio_net.ko \
         net/packet/af_packet.ko; do
    [ -f "$MODS/$m" ] && cp "$MODS/$m" "$WORK/root/lib/"
done

cp "$HERE/guest-init"      "$WORK/root/init"
cp "$HERE/udhcpc.script"   "$WORK/root/usr/share/udhcpc/default.script"
chmod +x "$WORK/root/init" "$WORK/root/usr/share/udhcpc/default.script"

mkdir -p "$(dirname "$OUT")"
( cd "$WORK/root" && find . | cpio -o -H newc --quiet | gzip -1 ) > "$OUT"
echo "built $OUT (kernel modules from $KREL)"
