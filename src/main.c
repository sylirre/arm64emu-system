/* Entry point: CLI parsing, image loading, and the run loop. */
#include "machine.h"
#include "cpu.h"
#include "tty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>

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
void machine_reset(Machine *m, u64 entry, unsigned reset_el) __attribute__((weak));

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
        "          [-drive IMG[,ro] (repeatable)] [-net] [-netfwd tcp|udp:HOST_PORT:GUEST_PORT]\n"
        "          [-virtfs DIR[,tag=TAG][,ro] (repeatable)] [-console pl011|virtio]\n"
        "          [-m MB] [-bin FLAT@ADDR] [-entry ADDR] [-el N] [-d] [-maxinsn N]\n", p);
}

/* True if host paths a and b name the same file. Compares canonical (realpath)
 * forms when both resolve; otherwise falls back to string equality so paths that
 * don't exist yet are still compared (realpath fails on a missing path). */
static bool same_host_path(const char *a, const char *b) {
    char ca[PATH_MAX], cb[PATH_MAX];
    const char *pa = realpath(a, ca) ? ca : a;
    const char *pb = realpath(b, cb) ? cb : b;
    return !strcmp(pa, pb);
}

int main(int argc, char **argv) {
    const char *bios = NULL, *kernel = NULL, *initrd = NULL, *append = "";
    const char *binfile = NULL, *dtbfile = NULL;
    Drive drives[MAX_DRIVES]; int n_drives = 0;
    VirtFS shares[MAX_SHARES]; int n_shares = 0;
    bool net_enabled = false;
    bool console_virtio = false;         /* -console: false=pl011 (default), true=virtio */
    NetFwd net_fwds[16]; int n_net_fwds = 0;
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
        else if (!strcmp(argv[i], "-drive") && i + 1 < argc) {
            /* Format: IMG[,ro][,rw]. Default is read-write. */
            if (n_drives >= MAX_DRIVES) { fprintf(stderr, "too many -drive disks\n"); return 1; }
            char *s = argv[++i];                 /* argv is mutable; split in place */
            char *path = s;
            bool ro = false;
            char *opt = strchr(s, ',');
            if (opt) *opt = '\0';                /* terminate IMG at first comma   */
            while (opt) {                        /* walk the ,opt,opt... suffix    */
                char *cur = opt + 1;
                char *nxt = strchr(cur, ',');
                if (nxt) *nxt = '\0';
                if      (!strcmp(cur, "ro")) ro = true;
                else if (!strcmp(cur, "rw")) ro = false;
                else { fprintf(stderr, "-drive: unknown option '%s'\n", cur); return 1; }
                opt = nxt;
            }
            if (!path[0]) { fprintf(stderr, "-drive: empty path\n"); return 1; }
            for (int d = 0; d < n_drives; d++)
                if (same_host_path(drives[d].path, path)) {
                    fprintf(stderr, "-drive: duplicate image '%s'\n", path); return 1;
                }
            drives[n_drives].path = path;
            drives[n_drives].ro   = ro;
            n_drives++;
        }
        else if (!strcmp(argv[i], "-virtfs") && i + 1 < argc) {
            /* Format: PATH[,tag=TAG][,ro]. Default tag = basename of PATH. */
            if (n_shares >= MAX_SHARES) { fprintf(stderr, "too many -virtfs shares\n"); return 1; }
            char *s = argv[++i];                 /* argv is mutable; split in place */
            char *path = s;
            char *tag = NULL;
            bool ro = false;
            char *opt = strchr(s, ',');
            if (opt) *opt = '\0';                /* terminate PATH at first comma  */
            while (opt) {                        /* walk the ,opt,opt... suffix    */
                char *cur = opt + 1;
                char *nxt = strchr(cur, ',');
                if (nxt) *nxt = '\0';
                if (!strncmp(cur, "tag=", 4)) tag = cur + 4;
                else if (!strcmp(cur, "ro")) ro = true;
                else if (!strcmp(cur, "rw")) ro = false;
                else { fprintf(stderr, "-virtfs: unknown option '%s'\n", cur); return 1; }
                opt = nxt;
            }
            if (!path[0]) { fprintf(stderr, "-virtfs: empty path\n"); return 1; }
            if (!tag) {                          /* default tag = basename(PATH)   */
                size_t pl = strlen(path);
                while (pl > 1 && path[pl - 1] == '/') path[--pl] = '\0';  /* trim '/' */
                char *slash = strrchr(path, '/');
                tag = (slash && slash[1]) ? slash + 1 : path;
            }
            for (int s2 = 0; s2 < n_shares; s2++) {
                if (same_host_path(shares[s2].path, path)) {
                    fprintf(stderr, "-virtfs: duplicate path '%s'\n", path); return 1;
                }
                if (!strcmp(shares[s2].tag, tag)) {
                    fprintf(stderr, "-virtfs: duplicate tag '%s'\n", tag); return 1;
                }
            }
            shares[n_shares].path = path;
            shares[n_shares].tag  = tag;
            shares[n_shares].ro   = ro;
            n_shares++;
        }
        else if (!strcmp(argv[i], "-net")) net_enabled = true;
        else if (!strcmp(argv[i], "-console") && i + 1 < argc) {
            const char *k = argv[++i];
            if      (!strcmp(k, "pl011"))  console_virtio = false;
            else if (!strcmp(k, "virtio")) console_virtio = true;
            else { fprintf(stderr, "-console: expected pl011|virtio\n"); return 1; }
        }
        else if (!strcmp(argv[i], "-netfwd") && i + 1 < argc) {
            /* Format: tcp:HOST_PORT:GUEST_PORT or udp:HOST_PORT:GUEST_PORT */
            if (n_net_fwds >= 16) { fprintf(stderr, "too many -netfwd rules\n"); return 1; }
            char *s = argv[++i];
            bool is_udp = false;
            if (!strncmp(s, "tcp:", 4)) { is_udp = false; s += 4; }
            else if (!strncmp(s, "udp:", 4)) { is_udp = true; s += 4; }
            else { fprintf(stderr, "-netfwd: expected tcp:HP:GP or udp:HP:GP\n"); return 1; }
            char *colon = strchr(s, ':');
            if (!colon) { fprintf(stderr, "-netfwd: missing guest port\n"); return 1; }
            *colon = '\0';
            net_fwds[n_net_fwds].is_udp    = is_udp;
            net_fwds[n_net_fwds].host_port  = atoi(s);
            net_fwds[n_net_fwds].guest_port = atoi(colon + 1);
            n_net_fwds++;
        } else if (!strcmp(argv[i], "-bin") && i + 1 < argc) {
            /* FILE or FILE@ADDR */
            char *s = argv[++i];
            char *at = strchr(s, '@');
            if (at) { *at = 0; bin_addr = strtoull(at + 1, 0, 0); }
            binfile = s;
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

    Machine m;
    machine_init(&m, ram_mb << 20);
    /* consumed by platform_build (virtio-blk: slots 1,2,3,...; net is slot 0) */
    if (n_drives) { memcpy(m.drives, drives, n_drives * sizeof(drives[0])); m.n_drives = n_drives; }
    if (n_shares) { memcpy(m.shares, shares, n_shares * sizeof(shares[0])); m.n_shares = n_shares; }
    m.net_enabled = net_enabled;
    m.console_virtio = console_virtio;
    if (console_virtio)
        fprintf(stderr, "[virtio-console] guest console is hvc0 — "
                        "boot with -append \"console=hvc0\"\n");
    if (n_net_fwds) { memcpy(m.net_fwds, net_fwds, n_net_fwds * sizeof(NetFwd)); m.n_net_fwds = n_net_fwds; }

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
        if (r == STEP_HALT) {
            if (m.cpu.reset && machine_reset) {   /* PSCI SYSTEM_RESET: warm reboot */
                machine_reset(&m, entry, (unsigned)reset_el);
                /* machine_reset restores the built-in DTB; re-apply file-based
                 * overrides that live in RAM, which the previous OS reused. */
                if (dtbfile) { size_t n; u8 *d = read_file(dtbfile, &n); phys_write_blk(&m, RAM_BASE, d, n); free(d); }
                if (binfile) { size_t n; u8 *b = read_file(binfile, &n); phys_write_blk(&m, bin_addr, b, n); free(b); }
                continue;
            }
            break;
        }
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
