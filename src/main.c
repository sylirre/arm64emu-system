/* Entry point: CLI parsing, image loading, and the run loop. */
#include "machine.h"
#include "cpu.h"
#include "tty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Devices / platform wiring (added in M3). Declared weakly here so M0/M1 link. */
void platform_build(Machine *m) __attribute__((weak));
void platform_setup_boot(Machine *m, const char *kernel, const char *initrd,
                         const char *append) __attribute__((weak));
void machine_wait_for_event(Machine *m) __attribute__((weak));
void machine_tick(Machine *m) __attribute__((weak));

static u8 *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc(n > 0 ? (size_t)n : 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "short read on %s\n", path); exit(1);
    }
    fclose(f);
    if (len_out) *len_out = (size_t)n;
    return buf;
}

static void usage(const char *p) {
    fprintf(stderr,
        "usage: %s [-bios FW.fd] [-kernel Image] [-initrd cpio] [-append CMDLINE]\n"
        "          [-m MB] [-bin FLAT@ADDR] [-entry ADDR] [-el N] [-d] [-maxinsn N]\n", p);
}

int main(int argc, char **argv) {
    const char *bios = NULL, *kernel = NULL, *initrd = NULL, *append = "";
    const char *binfile = NULL, *dtbfile = NULL;
    u64 ram_mb = 1024;
    u64 entry = 0;
    int reset_el = 1;
    u64 bin_addr = RAM_BASE;
    u64 max_insn = 0;     /* 0 = unlimited */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-bios") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "-kernel") && i + 1 < argc) kernel = argv[++i];
        else if (!strcmp(argv[i], "-initrd") && i + 1 < argc) initrd = argv[++i];
        else if (!strcmp(argv[i], "-append") && i + 1 < argc) append = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) ram_mb = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-entry") && i + 1 < argc) entry = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-el") && i + 1 < argc) reset_el = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d")) g_trace = 1;
        else if (!strcmp(argv[i], "-rt")) g_rtrace = 1;
        else if (!strcmp(argv[i], "-maxinsn") && i + 1 < argc) max_insn = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-dtb") && i + 1 < argc) dtbfile = argv[++i];
        else if (!strcmp(argv[i], "-bin") && i + 1 < argc) {
            /* FILE or FILE@ADDR */
            char *s = argv[++i];
            char *at = strchr(s, '@');
            if (at) { *at = 0; bin_addr = strtoull(at + 1, 0, 0); }
            binfile = s;
        } else { usage(argv[0]); return 1; }
    }

    if (getenv("AEDBG")) g_dbg = atoi(getenv("AEDBG"));

    Machine m;
    machine_init(&m, ram_mb << 20);

    if (bios) {
        size_t n; u8 *fw = read_file(bios, &n);
        if (n > m.flash_size) n = m.flash_size;
        memcpy(m.flash, fw, n);
        free(fw);
        entry = entry ? entry : FLASH_BASE;   /* firmware resets at flash base */
    }

    if (binfile) {
        size_t n; u8 *b = read_file(binfile, &n);
        phys_write_blk(&m, bin_addr, b, n);
        free(b);
        entry = entry ? entry : bin_addr;
    }

    if (!bios && !binfile) { usage(argv[0]); return 1; }

    if ((bios || kernel) && platform_build) platform_build(&m);
    if (dtbfile) {                       /* override embedded DTB at RAM base */
        size_t n; u8 *d = read_file(dtbfile, &n);
        phys_write_blk(&m, RAM_BASE, d, n);
        free(d);
    }
    if (kernel && platform_setup_boot)
        platform_setup_boot(&m, kernel, initrd, append);

    cpu_reset(&m.cpu, entry, (unsigned)reset_el);

    tty_raw_enable();

    unsigned long ticker = 0;
    for (;;) {
        if (machine_tick && (++ticker & 0x3ff) == 0) machine_tick(&m);
        StepResult r = cpu_step(&m.cpu);
        if (r == STEP_HALT) break;
        if (m.cpu.halted) {
            if (!machine_wait_for_event) break;
            machine_wait_for_event(&m);
        }
        if (max_insn && m.cpu.icount >= max_insn) {
            fprintf(stderr, "[maxinsn reached at icount=%llu]\n",
                    (unsigned long long)m.cpu.icount);
            cpu_dump(&m.cpu);
            break;
        }
    }

    tty_raw_disable();
    machine_free(&m);
    return 0;
}
