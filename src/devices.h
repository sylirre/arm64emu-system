/* Platform device interfaces (QEMU 'virt', GICv2). */
#ifndef A64_DEVICES_H
#define A64_DEVICES_H

#include "types.h"
#include "machine.h"

/* virt memory map */
#define GICD_BASE   0x08000000ULL
#define GICC_BASE   0x08010000ULL
#define UART_BASE   0x09000000ULL
#define RTC_BASE    0x09010000ULL
#define FWCFG_BASE  0x09020000ULL

/* Interrupt IDs (GIC INTIDs). SPIs = 32 + DTB-spi-number. */
#define INTID_TIMER_VIRT 27   /* PPI 11 */
#define INTID_TIMER_PHYS 30   /* PPI 14 (NS EL1 physical timer) */
#define INTID_UART       33   /* SPI 1  */
#define INTID_RTC        34   /* SPI 2  */
#define INTID_VIRTIO0    48   /* SPI 16 (virtio-mmio slot 0) */
#define INTID_VIRTIO1    49   /* SPI 17 (virtio-mmio slot 1) */

/* ---- GICv2 ---- */
#define GIC_NUM_IRQS 256
typedef struct GIC {
    CPU *cpu;
    u32 d_ctlr, c_ctlr;
    u8  pmr;
    bool enabled[GIC_NUM_IRQS];
    bool pending[GIC_NUM_IRQS];
    bool active[GIC_NUM_IRQS];
    bool line[GIC_NUM_IRQS];
    bool cfg_edge[GIC_NUM_IRQS];
    u8   priority[GIC_NUM_IRQS];
    u8   target[GIC_NUM_IRQS];
} GIC;

GIC *gic_create(Machine *m);
void gic_set_irq(GIC *g, int intid, int level);
void gic_update(GIC *g);

/* ---- generic timer ---- */
typedef struct ARMTimer { CPU *cpu; GIC *gic; } ARMTimer;
ARMTimer *gtimer_create(Machine *m);
void timer_update(Machine *m);
u64  timer_next_deadline_ns(Machine *m);     /* host-ns until next fire, or ~0 */
u64  timer_next_deadline_ticks(Machine *m);  /* instructions until next fire, or ~0 */

/* ---- PL011 UART ---- */
typedef struct PL011 {
    GIC *gic;
    u32 imsc, ris;
    u8  rx_fifo[64]; int rx_head, rx_tail;
    u32 cr, lcr_h, ibrd, fbrd, ifls;
} PL011;
PL011 *pl011_create(Machine *m, GIC *gic);
void pl011_rx_poll(Machine *m);   /* feed host stdin into RX FIFO */

/* ---- PL031 RTC ---- */
typedef struct PL031 { GIC *gic; u32 mr, lr, cr, imsc, ris; } PL031;
PL031 *pl031_create(Machine *m, GIC *gic);

/* ---- fw_cfg ---- */
typedef struct FwCfg FwCfg;
FwCfg *fwcfg_create(Machine *m);
void fwcfg_add_file(FwCfg *f, const char *name, const void *data, u32 len);
void fwcfg_set_legacy_kernel(FwCfg *f, const void *kernel, u32 ksize,
                             const void *initrd, u32 isize,
                             const char *cmdline);

/* ---- virtio-blk (virtio-mmio slot N) ---- */
/* slot picks the MMIO window (0x0a000000 + slot*0x200) and IRQ (INTID_VIRTIO0 + slot). */
struct VirtIOBlk;
struct VirtIOBlk *virtio_blk_create(Machine *m, GIC *gic, const char *path, int slot);

/* ---- virtio-net (virtio-mmio slot 0) ---- */
struct VirtIONet;
struct VirtIONet *virtio_net_create(Machine *m, GIC *gic);
void              virtio_net_poll(struct VirtIONet *v);

/* ---- virtio-9p (virtio-mmio slot N) ---- */
struct VirtIO9P;
struct VirtIO9P *virtio_9p_create(Machine *m, GIC *gic, const char *root,
                                  const char *tag, int slot);

#endif /* A64_DEVICES_H */
