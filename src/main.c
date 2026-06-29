/* Entry point: CLI parsing, image loading, and the run loop. */
#include "machine.h"
#include "cpu.h"
#include "tty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

/* On SIGINT/SIGTERM (e.g. a wall-clock `timeout` during a long boot), restore
 * the terminal and flush the diagnostics so a killed run still yields a profile
 * and the last PC — important since WFI idle makes -maxinsn slow in wall time. */
static CPU *g_sig_cpu;
static void on_signal(int sig) {
    (void)sig;
    tty_raw_disable();
    if (g_sig_cpu) { cpu_dump(g_sig_cpu); ring_dump(); }
    prof_dump();
    _exit(0);
}

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

typedef struct BootConfig {
    const char *bios, *kernel, *initrd, *append;
    const char *binfile, *dtbfile;
    const char *drives[MAX_DRIVES];
    int n_drives;
    const char *share_path, *share_tag;
    bool net_enabled;
    NetFwd net_fwds[16];
    int n_net_fwds;
    u64 ram_mb;
    u64 entry;
    int reset_el;
    u64 bin_addr;
} BootConfig;

static void boot_machine(Machine *m, const BootConfig *cfg) {
    u64 entry = cfg->entry;

    machine_init(m, cfg->ram_mb << 20);
    if (cfg->n_drives) {
        memcpy(m->drives, cfg->drives, cfg->n_drives * sizeof(cfg->drives[0]));
        m->n_drives = cfg->n_drives;
    }
    m->net_enabled = cfg->net_enabled;
    if (cfg->n_net_fwds) {
        memcpy(m->net_fwds, cfg->net_fwds, cfg->n_net_fwds * sizeof(NetFwd));
        m->n_net_fwds = cfg->n_net_fwds;
    }
    m->share_path = cfg->share_path;
    m->share_tag = cfg->share_tag;

    if (cfg->bios) {
        size_t n; u8 *fw = read_file(cfg->bios, &n);
        if (n > m->flash_size) n = m->flash_size;
        memcpy(m->flash, fw, n);
        free(fw);
        entry = entry ? entry : FLASH_BASE;   /* firmware resets at flash base */
    }

    if (cfg->binfile) {
        size_t n; u8 *b = read_file(cfg->binfile, &n);
        phys_write_blk(m, cfg->bin_addr, b, n);
        free(b);
        entry = entry ? entry : cfg->bin_addr;
    }

    if ((cfg->bios || cfg->kernel) && platform_build) platform_build(m);
    if (cfg->dtbfile) {
        size_t n; u8 *d = read_file(cfg->dtbfile, &n);
        phys_write_blk(m, RAM_BASE, d, n);
        free(d);
    }
    if (cfg->kernel && platform_setup_boot)
        platform_setup_boot(m, cfg->kernel, cfg->initrd, cfg->append);

    cpu_reset(&m->cpu, entry, (unsigned)cfg->reset_el);
}

static void usage(const char *p) {
    fprintf(stderr,
        "usage: %s [-bios FW.fd] [-kernel Image] [-initrd cpio] [-append CMDLINE]\n"
        "          [-drive IMG (repeatable)] [-share HOSTDIR[,tag=TAG]]\n"
        "          [-net] [-netfwd tcp|udp:HOST_PORT:GUEST_PORT]\n"
        "          [-m MB] [-bin FLAT@ADDR] [-entry ADDR] [-el N] [-d] [-maxinsn N]\n", p);
}

int main(int argc, char **argv) {
    BootConfig cfg = { .append = "", .share_tag = "hostshare", .ram_mb = 1024,
                       .reset_el = 1, .bin_addr = RAM_BASE };
    u64 max_insn = 0;     /* 0 = unlimited */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-bios") && i + 1 < argc) cfg.bios = argv[++i];
        else if (!strcmp(argv[i], "-kernel") && i + 1 < argc) cfg.kernel = argv[++i];
        else if (!strcmp(argv[i], "-initrd") && i + 1 < argc) cfg.initrd = argv[++i];
        else if (!strcmp(argv[i], "-append") && i + 1 < argc) cfg.append = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) cfg.ram_mb = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-entry") && i + 1 < argc) cfg.entry = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-el") && i + 1 < argc) cfg.reset_el = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d")) g_trace = 1;
        else if (!strcmp(argv[i], "-rt")) g_rtrace = 1;
        else if (!strcmp(argv[i], "-maxinsn") && i + 1 < argc) max_insn = strtoull(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-dtb") && i + 1 < argc) cfg.dtbfile = argv[++i];
        else if (!strcmp(argv[i], "-drive") && i + 1 < argc) {
            if (cfg.n_drives >= MAX_DRIVES) { fprintf(stderr, "too many -drive disks\n"); return 1; }
            cfg.drives[cfg.n_drives++] = argv[++i];
        }
        else if (!strcmp(argv[i], "-share") && i + 1 < argc) {
            char *s = argv[++i];
            char *comma = strstr(s, ",tag=");
            if (comma) {
                *comma = '\0';
                cfg.share_tag = comma + 5;
                if (!*cfg.share_tag) { fprintf(stderr, "-share: empty tag\n"); return 1; }
            }
            cfg.share_path = s;
            if (!*cfg.share_path) { fprintf(stderr, "-share: empty host directory\n"); return 1; }
        }
        else if (!strcmp(argv[i], "-net")) cfg.net_enabled = true;
        else if (!strcmp(argv[i], "-netfwd") && i + 1 < argc) {
            /* Format: tcp:HOST_PORT:GUEST_PORT or udp:HOST_PORT:GUEST_PORT */
            if (cfg.n_net_fwds >= 16) { fprintf(stderr, "too many -netfwd rules\n"); return 1; }
            char *s = argv[++i];
            bool is_udp = false;
            if (!strncmp(s, "tcp:", 4)) { is_udp = false; s += 4; }
            else if (!strncmp(s, "udp:", 4)) { is_udp = true; s += 4; }
            else { fprintf(stderr, "-netfwd: expected tcp:HP:GP or udp:HP:GP\n"); return 1; }
            char *colon = strchr(s, ':');
            if (!colon) { fprintf(stderr, "-netfwd: missing guest port\n"); return 1; }
            *colon = '\0';
            cfg.net_fwds[cfg.n_net_fwds].is_udp    = is_udp;
            cfg.net_fwds[cfg.n_net_fwds].host_port  = atoi(s);
            cfg.net_fwds[cfg.n_net_fwds].guest_port = atoi(colon + 1);
            cfg.n_net_fwds++;
        } else if (!strcmp(argv[i], "-bin") && i + 1 < argc) {
            /* FILE or FILE@ADDR */
            char *s = argv[++i];
            char *at = strchr(s, '@');
            if (at) { *at = 0; cfg.bin_addr = strtoull(at + 1, 0, 0); }
            cfg.binfile = s;
        } else { usage(argv[0]); return 1; }
    }

    if (getenv("AEDBG")) g_dbg = atoi(getenv("AEDBG"));
    if (getenv("AEPROF")) g_prof = atoi(getenv("AEPROF"));
    if (getenv("AETPC")) g_tpc = strtoull(getenv("AETPC"), 0, 0);
    if (getenv("AERING")) g_ring = atoi(getenv("AERING"));
    if (getenv("AEWATCH")) g_watch = strtoull(getenv("AEWATCH"), 0, 0);
    if (getenv("AEIABORT")) g_iabort_log = atoi(getenv("AEIABORT"));
    if (getenv("AE_RTCLOCK")) g_rtclock = atoi(getenv("AE_RTCLOCK"));
    if (getenv("AEVAW")) { g_vawatch = strtoull(getenv("AEVAW"), 0, 0); g_ring = 1; }
    if (getenv("AESYS")) g_systrace = atoi(getenv("AESYS"));
    if (getenv("AEHEAP")) { g_heaptrack = atoi(getenv("AEHEAP")); g_ring = 1; heaptrack_init(); }
    if (getenv("AEHEAP_AT")) g_heap_at = strtoull(getenv("AEHEAP_AT"), 0, 0);
    if (getenv("AECOV")) { g_ring = 1; cov_load(getenv("AECOV")); }
    /* Any per-instruction debug facility routes through the single hot-path guard.
     * (g_ring is also set by AECOV, so the coverage finder is covered too.) */
    g_debug_hooks = g_trace | g_rtrace | g_prof | g_ring | (g_tpc != 0);

    if (!cfg.bios && !cfg.binfile) { usage(argv[0]); return 1; }

    Machine m;
    boot_machine(&m, &cfg);
    g_sig_cpu = &m.cpu;
    tty_raw_enable();
    /* Register after tty_raw_enable() so these win over tty.c's cleanup-only handlers. */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    unsigned long ticker = 0;
    unsigned long tick_mask = 0x3ff;     /* AETICK: IRQ-poll granularity (debug) */
    if (getenv("AETICK")) tick_mask = strtoul(getenv("AETICK"), 0, 0);
    for (;;) {
        if (machine_tick && (++ticker & tick_mask) == 0) machine_tick(&m);
        StepResult r = cpu_step(&m.cpu);
        if (m.cpu.reset_request) {
            fprintf(stderr, "[reboot]\n");
            machine_free(&m);
            boot_machine(&m, &cfg);
            g_sig_cpu = &m.cpu;
            ticker = 0;
            continue;
        }
        if (r == STEP_HALT) break;
        if (m.cpu.halted) {
            if (!machine_wait_for_event) break;
            machine_wait_for_event(&m);
        }
        if (max_insn && m.cpu.icount >= max_insn) {
            fprintf(stderr, "[maxinsn reached at icount=%llu]\n",
                    (unsigned long long)m.cpu.icount);
            cpu_dump(&m.cpu);
            ring_dump();
            break;
        }
    }

    tty_raw_disable();
    prof_dump();
    if (g_heaptrack) heaptrack_report();
    machine_free(&m);
    return 0;
}
