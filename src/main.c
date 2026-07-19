/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Sylirre */
/* Entry point: CLI parsing, image loading, and the run loop.
 *
 *   arm64emu (--bios FW.fd | --bin FILE[@ADDR]) [options]
 *
 * The full option / environment-variable reference lives in help() below
 * (reachable via -h/--help), which reflows it to the terminal width. Keep the
 * option strings there and the overview in README.md in sync.
 */
#include "machine.h"
#include "cpu.h"
#include "tty.h"
#include "jit/jit.h"
#include "jit/predecode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <limits.h>

#define PROGRAM_VERSION "1.0.0"

/* On SIGINT/SIGTERM (e.g. a wall-clock `timeout` during a long boot), restore
 * the terminal and flush the diagnostics so a killed run still yields a profile
 * and the last PC — important since WFI idle makes --max-insn slow in wall time. */
static CPU *g_sig_cpu;
static void on_signal(int sig) {
    (void)sig;
    tty_raw_disable();
    if (g_sig_cpu) { cpu_dump(g_sig_cpu); ring_dump(); }
    prof_dump();
    jit_stats_flush();     /* _exit skips atexit; flush AEJIT_STATS if on */
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

/* Terse synopsis for argument errors: one line to stderr, exit 2. The full
 * reference lives in help() below, reachable via -h/--help. */
static void usage(void) {
    fprintf(stderr,
            "usage: arm64emu (--bios FW.fd | --bin FILE[@ADDR]) [options]\n"
            "try 'arm64emu --help' for details\n");
    exit(2);
}

/* Print the program version to stdout and exit 0 (-v/--version). */
static void version(void) {
    fputs("Version: " PROGRAM_VERSION "\n", stdout);
    fputs("GitHub: https://github.com/sylirre/arm64emu\n", stdout);
    exit(0);
}

/* --- help renderer: reflow the reference to the terminal width ---------
 * Layout mirrors the proot-distro help renderer: UPPERCASE sections framed by
 * blank lines, a two-column name/description table (one blank line between
 * entries) that collapses to a stacked layout on narrow PTYs, minus coloring. */

#define HELP_MIN_COLS 32   /* clamp for very narrow phone PTYs           */
#define HELP_MAX_COLS 92   /* clamp so wide terminals stay readable      */
#define HELP_NARROW   60   /* below this, name+description stack vertically */

/* A named entry (option / argument / env var) with its description. */
struct help_def { const char *name, *desc; };

/* Terminal width for help output, clamped to [HELP_MIN_COLS, HELP_MAX_COLS].
 * Help is written to stdout, so probe stdout first, then stdin/stderr, then
 * $COLUMNS (for redirected runs), finally a sane default. */
static int help_cols(void) {
    struct winsize ws;
    static const int fds[] = { 1, 0, 2 };   /* stdout, stdin, stderr */
    for (int i = 0; i < 3; i++) {
        if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            int c = ws.ws_col;
            return c < HELP_MIN_COLS ? HELP_MIN_COLS
                 : c > HELP_MAX_COLS ? HELP_MAX_COLS : c;
        }
    }
    int c = 80;
    const char *cols = getenv("COLUMNS");
    if (cols && *cols) { int v = atoi(cols); if (v > 0) c = v; }
    return c < HELP_MIN_COLS ? HELP_MIN_COLS
         : c > HELP_MAX_COLS ? HELP_MAX_COLS : c;
}

/* Greedy word-wrap: emit *text* to *f* wrapped to *width* columns, every line
 * prefixed with *indent* spaces. Words are never split; a blank line in the
 * source ("\n\n") starts a new paragraph. When *skip_first* is set the leading
 * indent of the very first line is suppressed — the caller has already placed
 * the cursor at column *indent* (used by the two-column table for line 1). */
static void help_wrap(FILE *f, const char *text, int width, int indent,
                      int skip_first) {
    int avail = width - indent;
    if (avail < 8) avail = 8;
    const char *p = text;
    int col = 0;                 /* chars used on the current line (past indent) */
    int need_indent = !skip_first;
    while (*p) {
        if (p[0] == '\n' && p[1] == '\n') {   /* paragraph break */
            if (col) fputc('\n', f);
            fputc('\n', f);
            col = 0;
            need_indent = 1;
            p += 2;
            while (*p == ' ' || *p == '\n') p++;
            continue;
        }
        if (*p == ' ' || *p == '\n') { p++; continue; }
        const char *e = p;                    /* one word: [p, e) */
        while (*e && *e != ' ' && *e != '\n') e++;
        int wlen = (int)(e - p);
        if (col == 0) {
            if (need_indent) fprintf(f, "%*s", indent, "");
            fwrite(p, 1, wlen, f);
            col = wlen;
            need_indent = 1;      /* every subsequent line is indented */
        } else if (col + 1 + wlen <= avail) {
            fputc(' ', f);
            fwrite(p, 1, wlen, f);
            col += 1 + wlen;
        } else {
            fprintf(f, "\n%*s", indent, "");
            fwrite(p, 1, wlen, f);
            col = wlen;
        }
        p = e;
    }
    if (col) fputc('\n', f);
}

/* Render name/description pairs as an aligned two-column table, falling back
 * to a stacked layout (name on its own line, description indented below) when
 * the terminal is too narrow to give the description a usable column. Entries
 * are separated by one blank line (proot-distro options spacing). */
static void help_defs(FILE *f, const struct help_def *d, int n, int width) {
    size_t longest = 0;
    for (int i = 0; i < n; i++)
        if (strlen(d[i].name) > longest) longest = strlen(d[i].name);

    int cap = width / 3; if (cap < 16) cap = 16;
    int opt_col = (int)longest < cap ? (int)longest : cap;
    int desc_col = width - opt_col - 4;
    int stacked = width < HELP_NARROW || desc_col < 24;

    for (int i = 0; i < n; i++) {
        if (stacked) {
            fprintf(f, "  %s\n", d[i].name);
            help_wrap(f, d[i].desc, width, 4, 0);
        } else {
            int cont = 2 + opt_col + 2;   /* description column start */
            if ((int)strlen(d[i].name) <= opt_col) {
                /* Name and its 2-space pad place the cursor at column *cont*,
                 * so the first description line skips its own indent. */
                fprintf(f, "  %-*s  ", opt_col, d[i].name);
                help_wrap(f, d[i].desc, width, cont, 1);
            } else {
                fprintf(f, "  %s\n", d[i].name);
                help_wrap(f, d[i].desc, width, cont, 0);
            }
        }
        if (i != n - 1) fputc('\n', f);   /* one blank line between entries */
    }
}

/* Print shell examples, each prefixed with "  $ " and wrapped with a trailing
 * " \\" continuation so long command lines stay copy-pasteable. */
static void help_examples(FILE *f, const char *const *ex, int n, int width) {
    int avail = width - 6; if (avail < 8) avail = 8;   /* -4 prefix, -2 " \\" */
    for (int i = 0; i < n; i++) {
        const char *p = ex[i];
        int first = 1, col = 0;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char *e = p;
            while (*e && *e != ' ') e++;
            int wlen = (int)(e - p);
            if (col == 0) {
                fputs(first ? "  $ " : "    ", f);
                fwrite(p, 1, wlen, f);
                col = wlen;
            } else if (col + 1 + wlen <= avail) {
                fputc(' ', f);
                fwrite(p, 1, wlen, f);
                col += 1 + wlen;
            } else {
                fputs(" \\\n", f);
                fputs("    ", f);
                fwrite(p, 1, wlen, f);
                col = wlen;
                first = 0;
            }
            p = e;
        }
        fputc('\n', f);
    }
}

/* Blank line, an UPPERCASE section heading, then a blank line beneath it
 * (proot-distro section spacing; no rule, no color). */
static void help_section(FILE *f, const char *title) {
    fprintf(f, "\n%s\n\n", title);
}

/* Full reference help: purpose, usage, every option, environment variables,
 * and examples. Reflowed to the terminal width. Printed to stdout on
 * -h/--help, exit 0. */
static void help(void) {
    FILE *f = stdout;
    int w = help_cols();

    static const struct help_def opts[] = {
        {"-h, --help",  "Show this help and exit."},
        {"-v, --version", "Show version and exit."},
        {"    --bios FILE", "Load firmware FILE into the NOR flash and reset "
                        "at the flash base (e.g. EDK2 QEMU_EFI.fd)."},
        {"    --kernel FILE", "Linux kernel image, handed to the firmware over "
                        "fw_cfg (the EDK2 EFI-stub boot path)."},
        {"    --initrd FILE", "Initramfs image, handed to the firmware over "
                        "fw_cfg."},
        {"    --append CMDLINE", "Kernel command line for --kernel boots. A "
                        "--drive ISO/GRUB boot ignores it."},
        {"    --dtb FILE", "Override the embedded device tree with FILE "
                        "(loaded at the RAM base)."},
        {"-m, --memory MB", "Guest RAM size in MiB. Default 1024."},
        {"    --drive IMG[,ro][,rw]", "Attach IMG as a virtio-blk disk "
                        "(repeatable, up to 8). 'ro' opens the image read-only "
                        "and advertises a read-only disk; default read-write."},
        {"    --virtfs DIR[,tag=TAG][,ro]", "Share host directory DIR with the "
                        "guest over virtio-9p (repeatable, up to 8). 'tag' "
                        "names the mount, default the directory basename; 'ro' "
                        "makes the share read-only."},
        {"    --net",   "User-mode networking (virtio-net through the built-in "
                        "NAT stack)."},
        {"    --netfwd tcp|udp:HP:GP", "Forward host port HP to guest port GP "
                        "(repeatable, up to 16; needs --net)."},
        {"    --console pl011|virtio", "Guest console device. Default pl011 "
                        "(ttyAMA0); 'virtio' adds an hvc0 console whose window "
                        "size tracks the host terminal."},
        {"    --bin FILE[@ADDR]", "Load a flat binary at ADDR (default the RAM "
                        "base) and reset there (bare-metal tests)."},
        {"    --entry ADDR", "Override the reset PC."},
        {"    --el N",  "Exception level at reset. Default 1."},
        {"    --max-insn N", "Stop after N instructions and dump the CPU "
                        "state. 0 = unlimited (default)."},
        {"-d, --trace", "Per-instruction trace to stderr. Very verbose; forces "
                        "the interpreter."},
        {"    --reg-trace", "Compact per-instruction register trace for "
                        "differential debugging; forces the interpreter."},
        {"-j, --jit",   "Translate hot code to native on x86-64 and AArch64 "
                        "hosts. Falls back to the interpreter elsewhere."},
        {"    --pd",    "Direct-threaded predecoded interpreter tier "
                        "(portable). --jit wins when both are given."},
        {"    --",      "Stop option parsing (arm64emu takes no positional "
                        "arguments)."},
    };
    static const struct help_def env_tune[] = {
        {"AEJIT_MB=N",  "JIT code-cache size in MiB. Default 32, max 128."},
        {"AE_RTCLOCK=1", "Drive the generic timer and the RTC from the host "
                        "clock instead of the deterministic instruction count."},
        {"AETICK=N",    "IRQ-poll granularity mask for the run loop. Default "
                        "0x3ff (poll every 1024 steps)."},
    };
    static const struct help_def env_diag[] = {
        {"AEDBG=N",     "Targeted device/IRQ debug logging (GIC, timer, "
                        "fw_cfg, flash)."},
        {"AEPROF=1",    "Hot-PC profiler; the ranking is dumped at exit or on "
                        "SIGINT."},
        {"AERING=1",    "Ring buffer of recent instructions, dumped on "
                        "abort/exit."},
        {"AETPC=0xPC",  "Dump the CPU state each time the pc hits this "
                        "address."},
        {"AEWATCH=0xADDR", "Log writes touching the 8 bytes at physical "
                        "ADDR."},
        {"AEVAW=0xVA",  "Log writes whose range covers this virtual address "
                        "(implies AERING=1)."},
        {"AEIABORT=1",  "Log instruction aborts."},
        {"AESYS=1",     "Trace guest memory-management syscalls "
                        "(mmap/brk/munmap/...)."},
        {"AEHEAP=1",    "musl-heap double-alloc finder (implies AERING=1)."},
        {"AEHEAP_AT=0xVA", "Log every alloc/free touching this virtual "
                        "address."},
        {"AECOV=FILE",  "Coverage-divergence finder: compare executed PCs "
                        "against a QEMU PC-set file."},
        {"AEJIT_STATS=1|/path", "Rank instruction words still run via the "
                        "exec_a64 helper, =/path dumps the ranking to a file "
                        "at exit."},
        {"AEJIT_DUMP=PREFIX", "Write each translated block into a sparse "
                        "code-cache image (PREFIX.code plus a .map)."},
        {"AEJIT_PDMAX=N", "Translate only PD ops <= N natively (bisects a "
                        "codegen bug to one instruction class)."},
        {"AEJIT_SLOWMEM=1", "Force every JIT memory access through the slow "
                        "helpers."},
        {"AEJIT_NOFUSE=1", "Disable instruction / D-TLB-probe fusion."},
        {"AEJIT_NOVRA=1", "Disable the V-register cache."},
        {"AEJIT_NOFP16=1", "Disable FP16 native codegen (AArch64 backend)."},
        {"AEJIT_SSE=2", "Force SSE2-baseline capability answers (x86-64)."},
        {"AEPD_MAX=N",  "Dispatch only PD ops <= N natively under --pd (0 = "
                        "pure interpreter); the --pd analogue of AEJIT_PDMAX."},
    };
    static const char *const examples[] = {
        "arm64emu --bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd",
        "arm64emu --bios QEMU_EFI.fd --kernel Image --initrd initramfs "
            "--append \"console=ttyAMA0 earlycon=pl011,0x9000000\"",
        "arm64emu --jit --net --bios QEMU_EFI.fd --kernel Image "
            "--initrd initramfs --virtfs /srv/data,tag=data,ro",
    };

    help_section(f, "USAGE");
    help_wrap(f, "arm64emu --bios FW.fd [--kernel Image --initrd initrd.img] [options]",
              w, 2, 0);
    help_wrap(f, "arm64emu --bin FILE[@ADDR] [options]", w, 2, 0);

    help_section(f, "DESCRIPTION");
    help_wrap(f,
        "AArch64 (ARMv8-A) full-system emulator modeling the QEMU 'virt' "
        "machine: GICv2, generic timer, PL011 UART, PL031 RTC, PSCI, fw_cfg, "
        "CFI NOR flash, and virtio-mmio (blk, net, 9p, console). Boots the "
        "real EDK2 ArmVirtQemu firmware and Linux to an interactive shell, "
        "with the serial console on your terminal.\n\n"
        "One of --bios or --bin is required. The plain interpreter is the "
        "reference engine; --pd and --jit are opt-in faster tiers kept "
        "byte-identical to it.",
    w, 2, 0);

    help_section(f, "OPTIONS");
    help_defs(f, opts, (int)(sizeof opts / sizeof *opts), w);

    help_section(f, "ENVIRONMENT");
    help_wrap(f, "Tuning:", w, 2, 0);
    fputc('\n', f);
    help_defs(f, env_tune, (int)(sizeof env_tune / sizeof *env_tune), w);
    fputc('\n', f);
    help_wrap(f, "Diagnostics, for development (the per-instruction "
                 "facilities force the interpreter):", w, 2, 0);
    fputc('\n', f);
    help_defs(f, env_diag, (int)(sizeof env_diag / sizeof *env_diag), w);

    help_section(f, "EXAMPLES");
    help_examples(f, examples, (int)(sizeof examples / sizeof *examples), w);
    fputc('\n', f);   /* trailing blank line so output ends clear of the prompt */

    exit(0);
}

/* --- command-line option parsing helpers ------------------------------- */

/* Fatal: a flag that takes no value was given one ("--help=x"). */
static void opt_no_value(const char *opt) {
    fprintf(stderr, "arm64emu: option '%s' takes no value\n", opt);
    exit(2);
}

/* Value for a long value-taking option: "--opt=VAL" (val, possibly "") if a '='
 * was present, else the next argv token (consumed via *pi). Fatal if none. */
static char *long_value(const char *opt, char *val, char **argv, int argc, int *pi) {
    if (val) return val;
    if (*pi + 1 < argc) return argv[++*pi];
    fprintf(stderr, "arm64emu: option '%s' requires an argument\n", opt);
    exit(2);
}

/* Value for a short value-taking option: the attached rest-of-token "-oVAL"
 * (rest) when non-empty, else the next argv token (consumed via *pi). */
static char *short_value(const char *opt, char *rest, char **argv, int argc, int *pi) {
    if (*rest) return rest;
    if (*pi + 1 < argc) return argv[++*pi];
    fprintf(stderr, "arm64emu: option '%s' requires an argument\n", opt);
    exit(2);
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

/* --drive IMG[,ro][,rw]: attach a virtio-blk disk. Default is read-write.
 * *spec* is an argv token, split in place; drives[].path points into argv. */
static void add_drive(Drive *drives, int *n_drives, char *spec) {
    if (*n_drives >= MAX_DRIVES) {
        fprintf(stderr, "arm64emu: too many --drive disks\n"); exit(2);
    }
    char *path = spec;
    bool ro = false;
    char *opt = strchr(spec, ',');
    if (opt) *opt = '\0';                /* terminate IMG at first comma   */
    while (opt) {                        /* walk the ,opt,opt... suffix    */
        char *cur = opt + 1;
        char *nxt = strchr(cur, ',');
        if (nxt) *nxt = '\0';
        if      (!strcmp(cur, "ro")) ro = true;
        else if (!strcmp(cur, "rw")) ro = false;
        else { fprintf(stderr, "arm64emu: --drive: unknown option '%s'\n", cur); exit(2); }
        opt = nxt;
    }
    if (!path[0]) { fprintf(stderr, "arm64emu: --drive: empty path\n"); exit(2); }
    for (int d = 0; d < *n_drives; d++)
        if (same_host_path(drives[d].path, path)) {
            fprintf(stderr, "arm64emu: --drive: duplicate image '%s'\n", path); exit(2);
        }
    drives[*n_drives].path = path;
    drives[*n_drives].ro   = ro;
    (*n_drives)++;
}

/* --virtfs PATH[,tag=TAG][,ro][,rw]: share a host directory over virtio-9p.
 * Default tag = basename of PATH. Split in place; fields point into argv. */
static void add_virtfs(VirtFS *shares, int *n_shares, char *spec) {
    if (*n_shares >= MAX_SHARES) {
        fprintf(stderr, "arm64emu: too many --virtfs shares\n"); exit(2);
    }
    char *path = spec;
    char *tag = NULL;
    bool ro = false;
    char *opt = strchr(spec, ',');
    if (opt) *opt = '\0';                /* terminate PATH at first comma  */
    while (opt) {                        /* walk the ,opt,opt... suffix    */
        char *cur = opt + 1;
        char *nxt = strchr(cur, ',');
        if (nxt) *nxt = '\0';
        if (!strncmp(cur, "tag=", 4)) tag = cur + 4;
        else if (!strcmp(cur, "ro")) ro = true;
        else if (!strcmp(cur, "rw")) ro = false;
        else { fprintf(stderr, "arm64emu: --virtfs: unknown option '%s'\n", cur); exit(2); }
        opt = nxt;
    }
    if (!path[0]) { fprintf(stderr, "arm64emu: --virtfs: empty path\n"); exit(2); }
    if (!tag) {                          /* default tag = basename(PATH)   */
        size_t pl = strlen(path);
        while (pl > 1 && path[pl - 1] == '/') path[--pl] = '\0';  /* trim '/' */
        char *slash = strrchr(path, '/');
        tag = (slash && slash[1]) ? slash + 1 : path;
    }
    for (int s2 = 0; s2 < *n_shares; s2++) {
        if (same_host_path(shares[s2].path, path)) {
            fprintf(stderr, "arm64emu: --virtfs: duplicate path '%s'\n", path); exit(2);
        }
        if (!strcmp(shares[s2].tag, tag)) {
            fprintf(stderr, "arm64emu: --virtfs: duplicate tag '%s'\n", tag); exit(2);
        }
    }
    shares[*n_shares].path = path;
    shares[*n_shares].tag  = tag;
    shares[*n_shares].ro   = ro;
    (*n_shares)++;
}

/* --netfwd tcp:HOST_PORT:GUEST_PORT or udp:HOST_PORT:GUEST_PORT. */
static void add_netfwd(NetFwd *fwds, int *n_fwds, char *spec) {
    if (*n_fwds >= 16) {
        fprintf(stderr, "arm64emu: too many --netfwd rules\n"); exit(2);
    }
    char *s = spec;
    bool is_udp = false;
    if (!strncmp(s, "tcp:", 4)) { is_udp = false; s += 4; }
    else if (!strncmp(s, "udp:", 4)) { is_udp = true; s += 4; }
    else { fprintf(stderr, "arm64emu: --netfwd: expected tcp:HP:GP or udp:HP:GP\n"); exit(2); }
    char *colon = strchr(s, ':');
    if (!colon) { fprintf(stderr, "arm64emu: --netfwd: missing guest port\n"); exit(2); }
    *colon = '\0';
    fwds[*n_fwds].is_udp     = is_udp;
    fwds[*n_fwds].host_port  = atoi(s);
    fwds[*n_fwds].guest_port = atoi(colon + 1);
    (*n_fwds)++;
}

int main(int argc, char **argv) {
    const char *bios = NULL, *kernel = NULL, *initrd = NULL, *append = "";
    const char *binfile = NULL, *dtbfile = NULL;
    Drive drives[MAX_DRIVES]; int n_drives = 0;
    VirtFS shares[MAX_SHARES]; int n_shares = 0;
    bool net_enabled = false;
    bool console_virtio = false;         /* --console: false=pl011 (default), true=virtio */
    NetFwd net_fwds[16]; int n_net_fwds = 0;
    u64 ram_mb = 1024;
    u64 entry = 0;
    int reset_el = 1;
    u64 bin_addr = RAM_BASE;
    u64 max_insn = 0;     /* 0 = unlimited */

    /* GNU-style options: single-letter short (-j), --word long. Value-taking
     * options accept "--opt VAL"/"--opt=VAL" and "-m VAL"/"-mVAL"; no-arg
     * shorts bundle ("-dj"). arm64emu takes no positional arguments. */
    int i = 1;
    for (; i < argc; i++) {
        char *arg = argv[i];
        if (arg[0] != '-' || arg[1] == '\0') break;   /* non-option: error below */
        if (!strcmp(arg, "--")) { i++; break; }        /* explicit end of options */

        if (arg[1] == '-') {                           /* long option: --name[=val] */
            char *eq = strchr(arg, '='), *val = NULL;
            if (eq) { *eq = '\0'; val = eq + 1; }      /* argv is writable */
            const char *n = arg + 2;
            if      (!strcmp(n, "help"))      { if (val) opt_no_value(arg); help(); }
            else if (!strcmp(n, "version"))   { if (val) opt_no_value(arg); version(); }
            else if (!strcmp(n, "trace"))     { if (val) opt_no_value(arg); g_trace = 1; }
            else if (!strcmp(n, "reg-trace")) { if (val) opt_no_value(arg); g_rtrace = 1; }
            else if (!strcmp(n, "jit"))       { if (val) opt_no_value(arg); g_jit = 1; }
            else if (!strcmp(n, "pd"))        { if (val) opt_no_value(arg); g_pd = 1; }
            else if (!strcmp(n, "net"))       { if (val) opt_no_value(arg); net_enabled = true; }
            else if (!strcmp(n, "bios"))     bios    = long_value("--bios", val, argv, argc, &i);
            else if (!strcmp(n, "kernel"))   kernel  = long_value("--kernel", val, argv, argc, &i);
            else if (!strcmp(n, "initrd"))   initrd  = long_value("--initrd", val, argv, argc, &i);
            else if (!strcmp(n, "append"))   append  = long_value("--append", val, argv, argc, &i);
            else if (!strcmp(n, "dtb"))      dtbfile = long_value("--dtb", val, argv, argc, &i);
            else if (!strcmp(n, "memory"))   ram_mb  = strtoull(long_value("--memory", val, argv, argc, &i), 0, 0);
            else if (!strcmp(n, "entry"))    entry   = strtoull(long_value("--entry", val, argv, argc, &i), 0, 0);
            else if (!strcmp(n, "el"))       reset_el = atoi(long_value("--el", val, argv, argc, &i));
            else if (!strcmp(n, "max-insn")) max_insn = strtoull(long_value("--max-insn", val, argv, argc, &i), 0, 0);
            else if (!strcmp(n, "drive"))    add_drive(drives, &n_drives, long_value("--drive", val, argv, argc, &i));
            else if (!strcmp(n, "virtfs"))   add_virtfs(shares, &n_shares, long_value("--virtfs", val, argv, argc, &i));
            else if (!strcmp(n, "netfwd"))   add_netfwd(net_fwds, &n_net_fwds, long_value("--netfwd", val, argv, argc, &i));
            else if (!strcmp(n, "console")) {
                const char *k = long_value("--console", val, argv, argc, &i);
                if      (!strcmp(k, "pl011"))  console_virtio = false;
                else if (!strcmp(k, "virtio")) console_virtio = true;
                else { fprintf(stderr, "arm64emu: --console: expected pl011|virtio\n"); exit(2); }
            }
            else if (!strcmp(n, "bin")) {              /* FILE or FILE@ADDR */
                char *s = long_value("--bin", val, argv, argc, &i);
                char *at = strchr(s, '@');
                if (at) { *at = 0; bin_addr = strtoull(at + 1, 0, 0); }
                binfile = s;
            }
            else usage();
        } else {                                       /* short cluster: -abc */
            for (char *p = arg + 1; *p; ) {
                char c = *p++;
                if      (c == 'h') help();
                else if (c == 'v') version();
                else if (c == 'd') g_trace = 1;
                else if (c == 'j') g_jit = 1;
                else if (c == 'm') { ram_mb = strtoull(short_value("--memory", p, argv, argc, &i), 0, 0); break; }
                else usage();
            }
        }
    }
    if (i < argc) {
        fprintf(stderr, "arm64emu: unexpected argument '%s'\n", argv[i]);
        usage();
    }
    if (ram_mb == 0) { fprintf(stderr, "arm64emu: --memory: invalid size\n"); exit(2); }

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

    /* The JIT batches instructions, so anything per-instruction forces the
     * interpreter: debug hooks (trace/rtrace/prof/ring/tpc/AECOV) and the
     * memory watchpoints (which also disable the D-TLB fast path). */
    if (g_jit && (g_debug_hooks || g_watch || g_vawatch)) {
        fprintf(stderr, "[jit] disabled: per-instruction debug facility active\n");
        g_jit = 0;
    }
    if (g_jit && !jit_backend_available()) {
        fprintf(stderr, "[jit] disabled: no code generator for this host\n");
        g_jit = 0;
    }
    /* --pd: opt-in direct-threaded interpreter tier. --jit wins if both given;
     * force it off for the per-instruction debug facilities (like --jit), which
     * expect one exec_a64 per step. */
    if (g_pd && g_jit) g_pd = 0;
    if (g_pd && (g_debug_hooks || g_watch || g_vawatch)) {
        fprintf(stderr, "[pd] disabled: per-instruction debug facility active\n");
        g_pd = 0;
    }

    Machine m;
    machine_init(&m, ram_mb << 20);
    /* consumed by platform_build (virtio-blk: slots 1,2,3,...; net is slot 0) */
    if (n_drives) { memcpy(m.drives, drives, n_drives * sizeof(drives[0])); m.n_drives = n_drives; }
    if (n_shares) { memcpy(m.shares, shares, n_shares * sizeof(shares[0])); m.n_shares = n_shares; }
    m.net_enabled = net_enabled;
    m.console_virtio = console_virtio;
    if (console_virtio) {
        if (kernel) {
            /* We control the cmdline on the --kernel path: make hvc0 the guest
             * console (last console= wins) so the login lands on hvc0 and host
             * I/O is effectively exclusive to the virtio-console. Skipped if the
             * user already asked for it. (A --drive ISO/GRUB boot ignores
             * --append, so the guest bootloader must set console=hvc0 itself.) */
            if (!strstr(append, "console=hvc0")) {
                static char append_hvc0[2048];
                snprintf(append_hvc0, sizeof append_hvc0, "%s%sconsole=hvc0", append,
                         (append[0] && append[strlen(append) - 1] != ' ') ? " " : "");
                append = append_hvc0;
            }
            fprintf(stderr, "[virtio-console] guest console -> hvc0 "
                            "(console=hvc0 added to the kernel cmdline)\n");
        } else {
            fprintf(stderr, "[virtio-console] guest console is hvc0 — set "
                            "console=hvc0 in the guest bootloader (e.g. GRUB) to use it\n");
        }
    }
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

    if (!bios && !binfile) usage();

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
        /* --jit/--pd: one step covers up to a tick-slice of instructions, so
         * tick on every return; the plain interpreter ticks every 1024 steps. */
        if (machine_tick && (g_jit || g_pd || (++ticker & tick_mask) == 0)) machine_tick(&m);
        StepResult r = g_jit ? jit_step(&m.cpu, tick_mask + 1, max_insn)
                     : g_pd  ? pd_step(&m.cpu, tick_mask + 1, max_insn)
                             : cpu_step(&m.cpu);
        if (r == STEP_HALT) {
            if (m.cpu.reset && machine_reset) {   /* PSCI SYSTEM_RESET: warm reboot */
                jit_reset();                      /* drop all translations */
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
