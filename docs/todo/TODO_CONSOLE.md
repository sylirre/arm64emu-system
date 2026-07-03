# arm64emu — virtio-console with host winsize propagation (implementation plan)

Goal: guest terminal size follows the host terminal automatically (no `resize`),
via a **virtio-console** device advertising `VIRTIO_CONSOLE_F_SIZE`, selectable
with a new CLI option:

```
-console pl011    # default — current behavior, byte-pipe serial console
-console virtio   # virtio-console (guest hvc0), host winsize propagated
```

Written 2026-07-03 (HEAD 2563105). Companion docs: BUGS/OPCODES/OPTIMIZATIONS
TODOs at repo root.

---

## 1. How it works (mechanism recap)

virtio-console (virtio DeviceID **3**), **non-multiport** (we do NOT advertise
`F_MULTIPORT`, avoiding control queues entirely):

- 2 virtqueues: **queue 0 = RX** (guest posts writable buffers, device fills
  with host keystrokes), **queue 1 = TX** (guest posts readable buffers with
  console output).
- Config space (LE): `struct virtio_console_config { u16 cols; u16 rows;
  u32 max_nr_ports; u32 emerg_wr; }`. We fill cols/rows (+ max_nr_ports=1).
- Features: `VIRTIO_F_VERSION_1` (bit 32) | `VIRTIO_CONSOLE_F_SIZE` (bit 0).
- With F_SIZE negotiated, Linux's `virtio_console` driver applies cols/rows to
  the hvc0 tty at probe, and re-reads them on every **config-change interrupt**
  (`InterruptStatus` bit 1), delivering SIGWINCH to the foreground process.
- Host resize: SIGWINCH → update config bytes → `isr |= 2` → raise the IRQ.

Guest boots with `console=hvc0` (keep `earlycon=pl011,0x9000000` — virtio
probes late, PL011 still exists as ttyAMA0 for early output).

## 2. Files touched

| File | Change |
|---|---|
| `src/devices/virtio_console.c` | **new** (~350 lines, modeled on virtio_blk.c/virtio_net.c) |
| `src/devices.h` | prototypes + `struct VirtIOConsole` fwd decl |
| `src/machine.h` | `struct VirtIOConsole *vcon;` + `bool console_virtio;` |
| `src/main.c` | parse `-console pl011\|virtio`; usage text |
| `src/platform.c` | create device, slot math, stub conditions, tick routing, machine_reset hook |
| `src/tty.c` / `src/tty.h` | `tty_get_winsize()`, SIGWINCH flag, optional `tty_write()` bulk output |
| `src/fdt/virt.dts` | **no change** (all 32 virtio-mmio slots already declared, edge IRQs) |
| `tests/asm/m12_vcon.S` | optional bare-metal probe test (§8) |

## 3. Step-by-step

### 3.1 CLI (`src/main.c`)
- Parse `-console <arg>`: `"pl011"` → default, `"virtio"` → `console_virtio = true`;
  anything else → usage error. Copy the `-drive` argv-parsing style
  (`main.c:90`). Store into `m.console_virtio` next to `m.net_enabled`
  (`main.c:201`).
- When virtio mode is chosen, print a boot hint to stderr:
  `[virtio-console] guest console is hvc0 — boot with -append "console=hvc0"`.

### 3.2 Host tty support (`src/tty.c`, `src/tty.h`)
- `void tty_get_winsize(unsigned short *cols, unsigned short *rows)`:
  `ioctl(STDOUT_FILENO, TIOCGWINSZ)`; on failure or `!isatty` return 0×0
  (winsize 0 means "unknown" — same as a serial line gives today, harmless).
- `volatile sig_atomic_t g_tty_winch` + a SIGWINCH handler that sets it;
  install in `tty_raw_enable()` (`tty.c:28`). No interaction with the existing
  SIGTERM/SIGSEGV cleanup handlers or main.c's SIGINT/SIGTERM — SIGWINCH is
  currently unused everywhere.
- Optional: `void tty_write(const void *buf, size_t len)` (single `write(2)`)
  so TX doesn't syscall per byte via `tty_putchar`.

### 3.3 The device (`src/devices/virtio_console.c`)
Clone the virtio-mmio register block from `virtio_blk.c:186-238` (read side:
Magic/Version=2/**DeviceID=3**/Vendor, features via `dev_feat_sel`,
QueueNumMax for q 0..1, QueueReady, ISR, Status, ConfigGeneration; write side:
feature/queue selectors, queue addresses lo/hi, QueueNotify, InterruptACK,
Status with reset-on-0) — the two-queue variant is in `virtio_net.c:344-410`,
which is the closer template (per-queue `q_num/q_ready/q_desc/q_avail/q_used/
last_avail[2]`).

State beyond the transport boilerplate:
```c
u16 cols, rows;                 /* current config-space values            */
u8  rx_fifo[1024]; int rx_head, rx_tail;   /* host input awaiting guest buffers */
```

**Config space read** (`off >= 0x100`): serialize `{cols, rows, max_nr_ports=1,
emerg_wr=0}` LE, byte-addressable, following `net_config_read`
(`virtio_net.c:332-342`). Config writes: ignore (we don't advertise
F_EMERG_WRITE).

**TX (QueueNotify val==1)**: drain the avail ring exactly like
`net_tx_process` (`virtio_net.c:171-217`): walk the chain **bounded by
`n >= q_num`**, gather readable descriptors, `tty_write()` the bytes,
`push_used(head, 0)`, then `isr |= 1` + `gic_set_irq(..., 1)` once.

**RX** — two halves, PL011-FIFO style plus `net_flush_rx` delivery:
1. *Poll* (called from machine_tick, §3.5): while FIFO not full and
   `tty_getchar() >= 0`, push into `rx_fifo`. (Stopping when full leaves
   backpressure in the host tty buffer — same contract as `pl011_rx_poll`,
   `pl011.c:33-39`.)
2. *Flush*: while FIFO non-empty and avail ring has buffers: pop the head
   descriptor chain, copy FIFO bytes into its **writable** descriptors,
   `push_used(head, written)`, repeat; then one `isr |= 1` + IRQ.
   Follow `net_flush_rx` (`virtio_net.c:121-167`) **but fix its two known
   bugs while copying** (BUGS doc §3.1/§3.2): bound the chain walk with a
   descriptor counter against `q_num`, skip descriptors lacking
   `VIRTQ_DESC_F_WRITE`, and push the *written* byte count, not the intended
   length.
3. QueueNotify val==0 (guest replenished RX buffers) → run the flush half
   immediately, mirroring the net RX notify path (`virtio_net.c:386`).

**Winsize update** (called from the poll entry point):
```c
if (g_tty_winch) {
    g_tty_winch = 0;
    tty_get_winsize(&c2, &r2);
    if (c2 != v->cols || r2 != v->rows) {
        v->cols = c2; v->rows = r2;
        v->isr |= 2;                       /* VIRTIO_MMIO_INT_CONFIG */
        gic_set_irq(v->gic, v->irq, 1);
    }
}
```
Initial size: `tty_get_winsize(&v->cols, &v->rows)` in `virtio_console_create`
— the driver reads config after DRIVER_OK, so it picks up the correct size at
first login with no interrupt needed.

**Reset** (`STATUS=0` write and `virtio_console_reset()` for warm reboot):
clear transport state per `net_reset` (`virtio_net.c:91-102`), drop the RX
FIFO, drop the IRQ line; re-read winsize so a reboot starts current.
InterruptACK handling: `isr &= ~val; if (!isr) gic_set_irq(...,0)` — same as
blk/net; note ACK must clear bit 1 too (the guest ACKs config interrupts with
value 2).

### 3.4 Slot allocation & stubs (`src/platform.c`)
- Console slot = `1 + n_drives + n_shares` (net=0 reserved, disks, shares,
  then console — keeps existing numbering deterministic). IRQ =
  `INTID_VIRTIO0 + slot`, MMIO base `0x0a000000 + slot*0x200` — the DTS
  already declares every slot with edge-triggered SPIs.
- Update the capacity check (`platform.c:71-75`):
  `n_drives + n_shares + (console_virtio ? 1 : 0) > 31`.
- Create after the 9p loop and **before** the empty-transport stubs (first-match
  dispatch in `find_dev` means real devices must register first —
  `platform.c:66-70` comment).
- Adjust the slot-1 stub condition (`platform.c:83`): only stub `0x0a000200`
  when no drive/share/console occupies slot 1.

### 3.5 Input routing (`machine_tick`, `src/platform.c:130-134`)
Dynamic handover so UEFI/early boot keeps keyboard input on PL011:
```c
if (m->console_virtio && m->vcon && vcon_driver_ok(m->vcon))
    virtio_console_poll(m->vcon);   /* stdin + winsize -> virtio-console */
else if (m->uart)
    pl011_rx_poll(m);               /* firmware / early boot / pl011 mode */
```
`vcon_driver_ok` = `(status & 4 /*DRIVER_OK*/) && q_ready[RX]`. Output needs no
routing — both devices write to the same host stdout, and the guest only
directs console output to one of them. In pl011 mode nothing changes at all
(flag false → identical behavior, preserving determinism of the default path).

`machine_reset` (`platform.c:107-128`): add
`if (m->vcon) virtio_console_reset(m->vcon);` next to the net reset — like
net, the console is background-polled so it must be quiesced explicitly.

### 3.6 Headers
- `devices.h`: `struct VirtIOConsole; VirtIOConsole *virtio_console_create(
  Machine*, GIC*, int slot); void virtio_console_poll(VirtIOConsole*);
  void virtio_console_reset(VirtIOConsole*); bool vcon_driver_ok(VirtIOConsole*);`
- `machine.h`: `struct VirtIOConsole *vcon;` + `bool console_virtio;`.

## 4. Boot usage (document in README)

```
./arm64emu -bios QEMU_EFI.fd -kernel Image.gz -initrd initrd \
    -console virtio -append "console=hvc0 earlycon=pl011,0x9000000"
```
- `console=hvc0` makes `/dev/console` (and the initramfs shell / getty) the
  virtio console; kernel early output still on earlycon.
- Users can also keep `console=ttyAMA0 console=hvc0` (last one wins for
  /dev/console) during transition.

## 5. Step 0 — verify guest kernel support (do this FIRST)

`CONFIG_VIRTIO_CONSOLE` (+ `CONFIG_HVC_DRIVER`) must be available. Known trap
from the 9p work: Alpine's initramfs lacks some virtio modules even when the
kernel packages them. Check before wiring anything:
- boot the current image unchanged, then in-guest:
  `zcat /proc/config.gz | grep -E 'VIRTIO_CONSOLE|HVC_DRIVER'` (or
  `grep virtio_console /lib/modules/*/modules.*`, or just
  `ls /sys/bus/virtio/drivers/`).
- If **=m and absent from initramfs**: `console=hvc0` yields a silent boot
  until rootfs. Mitigations: repack the initramfs with the module (recipe in
  the fpsimd memory note), or keep pl011 default (which this plan does).

## 6. Risks / contingencies

1. **Non-multiport + F_SIZE resize path**: modern kernels handle the
   non-multiport console resize via `config_work_handler` → hvc resize, but
   this is the less-traveled path (QEMU's virtconsole is multiport). If a
   target kernel only resizes multiport consoles, escalate: advertise
   `F_MULTIPORT` (bit 1), add queues 2/3 (control RX/TX) and the
   `PORT_ADD/PORT_OPEN/CONSOLE_PORT` handshake plus `VIRTIO_CONSOLE_RESIZE`
   control messages (~+150 lines). Decide after testing step 0's kernel —
   don't build it preemptively.
2. **UEFI menu input**: in virtio mode, stdin stays on PL011 until the guest
   driver is DRIVER_OK (§3.5), so firmware menus still work; after Linux takes
   over, ttyAMA0 input is dead (by design — the guest isn't reading it).
3. **Determinism**: default pl011 path is untouched. virtio mode adds host
   SIGWINCH/winsize as an input source — same nondeterminism class as
   keyboard input; acceptable, note in README.
4. **stdout not a tty** (piped runs, tests): winsize reads 0×0 and no SIGWINCH
   ever fires — equivalent to today's serial behavior; nothing breaks.

## 7. Suggested commit breakdown

1. `feat(tty): winsize query + SIGWINCH flag (+ bulk tty_write)` — inert.
2. `feat(virtio): virtio-console device (DeviceID 3, F_SIZE, non-multiport)`
   — device + headers + platform wiring behind `-console virtio`.
3. `feat(cli): -console pl011|virtio option (default pl011)` — parse + docs.
4. `test: m12 virtio-console probe` (optional, §8).

## 8. Verification plan

1. **Default regression**: `make test` (m1-m11) and a full firmware+kernel
   boot with no `-console` flag — must be byte-identical behavior (routing
   code compiles out to the old call).
2. **Device probe**: boot with `-console virtio`, `console=ttyAMA0` (console
   still serial): guest sees the device — `ls /sys/bus/virtio/devices/`,
   `dmesg | grep hvc`; `/dev/hvc0` exists.
3. **Console on hvc0**: boot `console=hvc0`; initramfs shell I/O works
   (typing, output); `stty size` reports the **host terminal dimensions**
   (today it reports `0 0` or `24 80`).
4. **Live resize**: resize the host terminal window; in-guest `stty size`
   reflects it without any command; `less`/`vi` redraw on SIGWINCH.
5. **Warm reboot**: PSCI SYSTEM_RESET (m11-style) with `-console virtio`;
   console works again after reboot (STATUS=0 re-probe path), no stale IRQ.
6. **Optional m12_vcon.S**: bare-metal — read DeviceID==3 at the console slot,
   check feature bit 0, negotiate, push one TX buffer ("OK\n") and expect it
   on stdout via the existing test harness output comparison. (cols/rows will
   read 0 under the non-tty test harness — assert only the mechanics.)

## 9. Effort estimate

~350 lines new device (mostly transplanted transport boilerplate), ~60 lines
across tty/platform/main/headers. One session including the guest-side
verification, assuming step 0 shows `virtio_console` is available.
