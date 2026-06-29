/* Platform wiring: instantiate virt devices, place the DTB, load the kernel
 * via fw_cfg, and drive timer/UART events for the run loop. */
#include "devices.h"
#include "machine.h"
#include "fdt/virt_dtb.h"
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <unistd.h>

static u8 *load_file(const char *path, u32 *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *b = malloc(n > 0 ? (size_t)n : 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read %s\n", path); exit(1); }
    fclose(f);
    *len = (u32)n;
    return b;
}

/* Stub MMIO: probed-but-unimplemented devices (virtio/pcie/gpio). Reads return
 * a fixed "absent" value so firmware/OS detect no device instead of faulting. */
static u64 stub_zero_read(void *o, u64 off, unsigned size) { return 0; }
static u64 stub_ones_read(void *o, u64 off, unsigned size) {
    return size >= 8 ? ~0ULL : ((1ULL << (size * 8)) - 1);
}
static void stub_write(void *o, u64 off, unsigned size, u64 v) { (void)o; (void)off; (void)size; (void)v; }

/* Empty virtio-mmio transport: QEMU's 'virt' instantiates 32 transport slots
 * (stride 0x200) even with no backend. An empty slot still answers the probe
 * registers with magic="virt", version=1 (legacy), DeviceID=0 (no device),
 * VendorID="QEMU"; the firmware/OS then skips it. Returning 0 here (as the old
 * stub did) makes firmware believe the transport is absent and take a divergent
 * path. We model the slots as empty (no device) until virtio-blk lands (M6). */
static u64 virtio_mmio_read(void *o, u64 off, unsigned size) {
    (void)o; (void)size;
    switch (off & 0x1ff) {            /* register within the 0x200-byte slot */
        case 0x000: return 0x74726976; /* VIRTIO_MMIO_MAGIC_VALUE "virt" */
        case 0x004: return 0x1;        /* VIRTIO_MMIO_VERSION (legacy)   */
        case 0x008: return 0x0;        /* VIRTIO_MMIO_DEVICE_ID = none    */
        case 0x00c: return 0x554d4551; /* VIRTIO_MMIO_VENDOR_ID "QEMU"    */
        default:    return 0;
    }
}

void platform_build(Machine *m) {
    gic_create(m);
    gtimer_create(m);
    pl011_create(m, m->gic);
    pl031_create(m, m->gic);
    fwcfg_create(m);

    /* GICv2m MSI frame: advertised in the device tree but not emulated. Returning
     * 0 for MSI_TYPER makes Linux's is_msi_spi_valid() fail gracefully, so the
     * kernel boots without MSI (virtio-mmio uses wired SPIs); an unmapped read
     * here would instead fault and panic early init. */
    machine_add_device(m, 0x08020000, 0x1000,       stub_zero_read, stub_write, m, "gicv2m");

    /* Absent-device stubs covering the QEMU 'virt' regions we don't model. */
    machine_add_device(m, 0x09030000, 0x1000,       stub_zero_read, stub_write, m, "gpio-stub");

    /* virtio-mmio: 32 slots (0x0a000000–0x0a003fff, stride 0x200).
     * Slot 0 is reserved for virtio-net (-net); disks (-drive) take slots 1,2,3,...
     * and virtio-9p (-share) follows the disks, so block-device numbering is
     * deterministic regardless of optional net/share devices.
     * Real devices must be registered before any stub that covers their range
     * because find_dev uses first-match dispatch — so a disk at slot 2+ shadows
     * the catch-all stub below for its own 0x200 window.
     * Unoccupied slots get empty-transport stubs (DeviceID=0). */
    if (m->net_enabled) virtio_net_create(m, m->gic);
    for (int i = 0; i < m->n_drives; i++)
        virtio_blk_create(m, m->gic, m->drives[i], i + 1);   /* slots 1,2,3,... */
    if (m->share_path)
        virtio_9p_create(m, m->gic, m->share_path, m->share_tag ? m->share_tag : "hostshare",
                         m->n_drives + 1);
    if (!m->net_enabled) machine_add_device(m, 0x0a000000, 0x200, virtio_mmio_read, stub_write, m, "virtio-mmio");
    if (m->n_drives == 0) machine_add_device(m, 0x0a000200, 0x200, virtio_mmio_read, stub_write, m, "virtio-mmio");
    machine_add_device(m, 0x0a000400, 0x3c00, virtio_mmio_read, stub_write, m, "virtio-mmio");
    machine_add_device(m, 0x3eff0000, 0x10000,      stub_ones_read, stub_write, m, "pcie-pio");
    machine_add_device(m, 0x10000000, 0x2eff0000,   stub_ones_read, stub_write, m, "pcie-mmio");
    machine_add_device(m, 0x4010000000ULL, 0x10000000, stub_ones_read, stub_write, m, "pcie-ecam");

    /* Place the device tree at the base of RAM, where EDK2 ArmVirtQemu expects
     * it (PcdDeviceTreeInitialBaseAddress == base of system memory). */
    phys_write_blk(m, RAM_BASE, virt_dtb, virt_dtb_len);
}

void platform_setup_boot(Machine *m, const char *kernel, const char *initrd,
                         const char *append) {
    if (!m->fwcfg) return;
    u8 *kdata = NULL, *idata = NULL;
    u32 klen = 0, ilen = 0;
    if (kernel) kdata = load_file(kernel, &klen);
    if (initrd) idata = load_file(initrd, &ilen);
    fwcfg_set_legacy_kernel(m->fwcfg, kdata, klen, idata, ilen, append ? append : "");
    fprintf(stderr, "[boot] kernel=%u bytes initrd=%u bytes cmdline=\"%s\"\n",
            klen, ilen, append ? append : "");
    free(kdata); free(idata);
}

void machine_tick(Machine *m) {
    if (m->timer) timer_update(m);
    if (m->uart) pl011_rx_poll(m);
    if (m->net) virtio_net_poll(m->net);
}

void machine_wait_for_event(Machine *m) {
    CPU *c = &m->cpu;

    if (!g_rtclock) {
        /* Deterministic timer: the counter only advances as instructions retire,
         * so while halted in WFI it would never reach the next deadline. Jump the
         * virtual clock forward by exactly the ticks remaining to the nearest
         * armed timer, so the IRQ fires at a fixed, reproducible icount. */
        u64 dl = timer_next_deadline_ticks(m);

        /* Service interactive input without perturbing the guest clock. On a tty
         * with no timer pending, block briefly so we don't busy-spin a host core;
         * otherwise just peek. Non-tty stdin (e.g. /dev/null) never has input. */
        int has_input = 0;
        if (isatty(STDIN_FILENO)) {
            struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
            has_input = poll(&pfd, 1, (dl == ~0ULL) ? 50 : 0) > 0;
        } else if (dl == ~0ULL && !c->irq_line) {
            poll(NULL, 0, 50);   /* truly idle, no timer armed: don't peg a core */
        }

        if (!c->irq_line && !has_input && dl != ~0ULL)
            c->timer_skip += dl;     /* fast-forward to the nearest timer deadline */

        machine_tick(m);             /* re-evaluate the timer line at the new count */
        if (c->irq_line || has_input) c->halted = false;
        return;
    }

    /* Real-time mode (AE_RTCLOCK): pace against the host monotonic clock. */
    u64 dl = timer_next_deadline_ns(m);
    int timeout_ms;
    if (dl == 0) timeout_ms = 0;
    else if (dl == ~0ULL) timeout_ms = 50;
    else { timeout_ms = (int)(dl / 1000000ULL); if (timeout_ms > 50) timeout_ms = 50; if (timeout_ms < 1) timeout_ms = 1; }

    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int r = isatty(STDIN_FILENO) ? poll(&pfd, 1, timeout_ms) : (timeout_ms ? (poll(NULL,0,timeout_ms),0) : 0);

    machine_tick(m);
    if (c->irq_line || r > 0) c->halted = false;
}
