/* Machine: physical memory bus (RAM + flash + MMIO devices). */
#ifndef A64_MACHINE_H
#define A64_MACHINE_H

#include "types.h"
#include "cpu.h"

/* Default platform constants (QEMU 'virt', GICv2). */
#define FLASH_BASE   0x00000000ULL
#define FLASH_SIZE   0x08000000ULL   /* 128 MB: 64 MB code + 64 MB vars */
#define FLASH_BANK   0x04000000ULL   /* 64 MB per bank */
#define RAM_BASE     0x40000000ULL
#define RAM_SIZE_DEF (1024ULL << 20) /* 1 GB default */

/* Memory abort reporting from the bus (unmapped access). */
typedef enum { BUS_OK = 0, BUS_FAULT = 1 } BusStatus;

typedef u64  (*mmio_read_fn)(void *opaque, u64 offset, unsigned size);
typedef void (*mmio_write_fn)(void *opaque, u64 offset, unsigned size, u64 value);

typedef struct {
    u64 base, size;
    mmio_read_fn read;
    mmio_write_fn write;
    void *opaque;
    const char *name;
} MMIODev;

#define MAX_DEVS 48

struct GIC;
struct PL011;
struct PL031;
struct FwCfg;
struct ARMTimer;

typedef struct Machine {
    CPU cpu;

    u8 *ram;  u64 ram_base, ram_size;
    u8 *flash; u64 flash_base, flash_size;
    bool flash_writable;          /* simple RAM-backed flash writes (CFI added later) */

    MMIODev dev[MAX_DEVS];
    int ndev;

    /* Device instances (owned). */
    struct GIC      *gic;
    struct ARMTimer *timer;
    struct PL011    *uart;
    struct PL031    *rtc;
    struct FwCfg    *fwcfg;

    BusStatus last_bus_status;    /* set by phys_* on fault */
} Machine;

void machine_init(Machine *m, u64 ram_size);
void machine_free(Machine *m);
void machine_add_device(Machine *m, u64 base, u64 size, mmio_read_fn r,
                        mmio_write_fn w, void *opaque, const char *name);

/* Physical memory access. On unmapped access, sets m->last_bus_status=BUS_FAULT
 * and returns 0 (reads) / ignores (writes). `size` is 1,2,4,8. */
u64  phys_read(Machine *m, u64 pa, unsigned size);
void phys_write(Machine *m, u64 pa, unsigned size, u64 value);

/* Bulk copy helpers (used by loaders / fw_cfg DMA). Truncated to mapped RAM. */
void phys_write_blk(Machine *m, u64 pa, const void *src, u64 len);
void phys_read_blk(Machine *m, u64 pa, void *dst, u64 len);

/* Direct host pointer into RAM for [pa, pa+len), or NULL if out of range. */
void *ram_ptr(Machine *m, u64 pa, u64 len);

#endif /* A64_MACHINE_H */
