/* virtio-console (DeviceID 3) bare-metal probe self-test.
 *
 * run_tests.sh runs this one as `-bios <img> -console virtio`, not `-bin`: the
 * virtio-console (like every virtio device) is created only when platform_build
 * runs, i.e. under -bios/-kernel — a plain -bin load has no MMIO devices at all.
 * So this executes from flash at 0x0, at EL1 with the MMU off, and uses physical
 * addresses directly for MMIO (0x0a000200 = slot 1) and for the virtqueue in RAM
 * (0x40200000+, past the DTB that platform_build placed at RAM_BASE 0x40000000).
 *
 * It asserts the transport mechanics only (cols/rows need a real tty and read 0
 * under the non-tty harness, so geometry is not checked):
 *   - MagicValue "virt", DeviceID 3
 *   - offered features include F_SIZE (bit 0) and VERSION_1 (bit 32)
 *   - negotiate + bring up the TX queue, push one "OK\n" buffer, notify, and
 *     confirm the device published it on the used ring (idx advanced to 1).
 * Position-independent (relative branches + movz/movk absolute addresses), so it
 * runs correctly from flash regardless of the link address the harness uses.
 *
 * x0=0 on success; a distinct nonzero code per failed step (HLT dumps x0).
 */

.text
.global _start
_start:
    /* x9 = virtio-console MMIO base. net is absent (slot 0 stub), no disks or
     * shares, so the console lands on slot 1 => 0x0a000000 + 1*0x200. */
    movz    x9, #0x0a00, lsl #16
    movk    x9, #0x0200                 /* 0x0a000200 */

    /* --- MagicValue == "virt" (0x74726976) --- */
    ldr     w0, [x9, #0x000]
    movz    w1, #0x6976
    movk    w1, #0x7472, lsl #16        /* 0x74726976 */
    cmp     w0, w1
    movz    x0, #10
    b.ne    done                        /* 10: bad magic */

    /* --- DeviceID == 3 (virtio-console) --- */
    ldr     w0, [x9, #0x008]
    cmp     w0, #3
    movz    x0, #11
    b.ne    done                        /* 11: wrong device id */

    /* --- offered features: sel 0 must expose F_SIZE (bit 0) --- */
    str     wzr, [x9, #0x014]           /* DeviceFeaturesSel = 0 */
    ldr     w0, [x9, #0x010]
    tst     w0, #1
    movz    x0, #12
    b.eq    done                        /* 12: F_SIZE not offered */

    /* --- offered features: sel 1 must expose VERSION_1 (bit 32 => hi bit 0) --- */
    movz    w1, #1
    str     w1, [x9, #0x014]            /* DeviceFeaturesSel = 1 */
    ldr     w0, [x9, #0x010]
    tst     w0, #1
    movz    x0, #13
    b.eq    done                        /* 13: VERSION_1 not offered */

    /* --- negotiate: ACKNOWLEDGE, then DRIVER --- */
    movz    w1, #1
    str     w1, [x9, #0x070]            /* Status = ACKNOWLEDGE */
    movz    w1, #3
    str     w1, [x9, #0x070]            /* Status |= DRIVER */

    /* accept VERSION_1 (sel 1, bit 0) and F_SIZE (sel 0, bit 0) */
    movz    w1, #1
    str     w1, [x9, #0x024]            /* DriverFeaturesSel = 1 */
    movz    w1, #1
    str     w1, [x9, #0x020]            /* DriverFeatures hi = VERSION_1 */
    str     wzr, [x9, #0x024]           /* DriverFeaturesSel = 0 */
    movz    w1, #1
    str     w1, [x9, #0x020]            /* DriverFeatures lo = F_SIZE */

    movz    w1, #0x0b
    str     w1, [x9, #0x070]            /* Status |= FEATURES_OK (1|2|8) */

    /* --- virtqueue memory (all in RAM, past the DTB at 0x40000000) --- */
    movz    x10, #0x4020, lsl #16       /* descs  = 0x40200000 */
    movz    x11, #0x4020, lsl #16
    movk    x11, #0x1000                /* availr = 0x40201000 */
    movz    x12, #0x4020, lsl #16
    movk    x12, #0x2000                /* usedr  = 0x40202000 */
    movz    x13, #0x4020, lsl #16
    movk    x13, #0x3000                /* databf = 0x40203000 */

    /* --- bring up TX queue (queue 1) --- */
    movz    w1, #1
    str     w1, [x9, #0x030]            /* QueueSel = 1 (TX) */
    movz    w1, #4
    str     w1, [x9, #0x038]            /* QueueNum = 4 */
    str     w10, [x9, #0x080]           /* QueueDescLow  */
    str     wzr, [x9, #0x084]           /* QueueDescHigh */
    str     w11, [x9, #0x090]           /* QueueAvailLow  */
    str     wzr, [x9, #0x094]           /* QueueAvailHigh */
    str     w12, [x9, #0x0a0]           /* QueueUsedLow  */
    str     wzr, [x9, #0x0a4]           /* QueueUsedHigh */
    movz    w1, #1
    str     w1, [x9, #0x044]            /* QueueReady = 1 */

    movz    w1, #0x0f
    str     w1, [x9, #0x070]            /* Status |= DRIVER_OK (1|2|4|8) */

    /* --- descriptor 0: addr=databf, len=3, flags=0 (readable), next=0 --- */
    str     x13, [x10]                  /* desc.addr (64-bit) */
    movz    w1, #3
    str     w1, [x10, #8]               /* desc.len = 3 */
    str     wzr, [x10, #12]             /* desc.flags=0, desc.next=0 */

    /* --- payload "OK\n" => 0x000a4b4f little-endian (len=3 consumed) --- */
    movz    w1, #0x4b4f                 /* 'O'=0x4f, 'K'=0x4b */
    movk    w1, #0x000a, lsl #16        /* '\n'=0x0a */
    str     w1, [x13]

    /* --- avail ring: flags=0, idx=1, ring[0]=0 --- */
    movz    w1, #1
    lsl     w1, w1, #16                 /* idx=1 in bits[31:16], flags=0 */
    str     w1, [x11]                   /* avail.flags @0, avail.idx @2 */
    str     wzr, [x11, #4]              /* avail.ring[0] = 0 */

    /* --- used ring: zero flags+idx so push_used starts from 0 --- */
    str     wzr, [x12]

    /* --- kick: QueueNotify = TX; the device drains it synchronously --- */
    movz    w1, #1
    str     w1, [x9, #0x050]

    /* --- used.idx must now be 1 (device consumed our buffer) --- */
    ldrh    w0, [x12, #2]
    cmp     w0, #1
    movz    x0, #14
    b.ne    done                        /* 14: buffer not consumed */

    movz    x0, #0                      /* all checks passed */
done:
    hlt     #0
