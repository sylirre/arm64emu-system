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

void platform_build(Machine *m) {
    gic_create(m);
    gtimer_create(m);
    pl011_create(m, m->gic);
    pl031_create(m, m->gic);
    fwcfg_create(m);

    /* Absent-device stubs covering the QEMU 'virt' regions we don't model. */
    machine_add_device(m, 0x09030000, 0x1000,       stub_zero_read, stub_write, m, "gpio-stub");
    machine_add_device(m, 0x0a000000, 0x4000,       stub_zero_read, stub_write, m, "virtio-stub");
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
}

void machine_wait_for_event(Machine *m) {
    CPU *c = &m->cpu;
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
