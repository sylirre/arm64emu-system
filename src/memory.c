/* Physical memory bus: RAM, flash, and MMIO device dispatch. */
#include "machine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void machine_init(Machine *m, u64 ram_size) {
    memset(m, 0, sizeof(*m));
    m->ram_base = RAM_BASE;
    m->ram_size = ram_size ? ram_size : RAM_SIZE_DEF;
    m->ram = calloc(1, m->ram_size);
    m->flash_base = FLASH_BASE;
    m->flash_size = FLASH_SIZE;
    m->flash = calloc(1, m->flash_size);
    m->flash_writable = true;   /* naive flash writes for now; CFI emulation TBD */
    if (!m->ram || !m->flash) {
        fprintf(stderr, "fatal: out of memory allocating guest RAM/flash\n");
        exit(1);
    }
    m->ndev = 0;
    m->cpu.m = m;
}

void machine_free(Machine *m) {
    free(m->ram);
    free(m->flash);
}

void machine_add_device(Machine *m, u64 base, u64 size, mmio_read_fn r,
                        mmio_write_fn w, void *opaque, const char *name) {
    if (m->ndev >= MAX_DEVS) {
        fprintf(stderr, "fatal: too many MMIO devices\n");
        exit(1);
    }
    MMIODev *d = &m->dev[m->ndev++];
    d->base = base; d->size = size; d->read = r; d->write = w;
    d->opaque = opaque; d->name = name;
}

static MMIODev *find_dev(Machine *m, u64 pa) {
    for (int i = 0; i < m->ndev; i++) {
        MMIODev *d = &m->dev[i];
        if (pa >= d->base && pa < d->base + d->size) return d;
    }
    return NULL;
}

void *ram_ptr(Machine *m, u64 pa, u64 len) {
    if (pa >= m->ram_base && pa + len <= m->ram_base + m->ram_size)
        return m->ram + (pa - m->ram_base);
    return NULL;
}

/* Host is little-endian; guest is little-endian. Direct memcpy preserves order. */
u64 phys_read(Machine *m, u64 pa, unsigned size) {
    m->last_bus_status = BUS_OK;
    if (g_dbg >= 2 && pa >= RAM_BASE && pa < RAM_BASE + 0x40) {
        static int n = 0;
        if (n++ < 40) fprintf(stderr, "[dtbrd] pa=0x%llx size=%u pc=0x%llx\n",
                              (unsigned long long)pa, size, (unsigned long long)m->cpu.cur_insn_pc);
    }
    u8 *p = NULL;
    if (pa >= m->ram_base && pa + size <= m->ram_base + m->ram_size)
        p = m->ram + (pa - m->ram_base);
    else if (pa >= m->flash_base && pa + size <= m->flash_base + m->flash_size)
        p = m->flash + (pa - m->flash_base);

    if (p) {
        u64 v = 0;
        memcpy(&v, p, size);
        return v;
    }
    MMIODev *d = find_dev(m, pa);
    if (d && d->read) return d->read(d->opaque, pa - d->base, size);

    m->last_bus_status = BUS_FAULT;
    if (g_trace)
        fprintf(stderr, "[bus] unmapped read  pa=0x%llx size=%u\n",
                (unsigned long long)pa, size);
    return 0;
}

void phys_write(Machine *m, u64 pa, unsigned size, u64 value) {
    m->last_bus_status = BUS_OK;
    if (g_dbg >= 3 && pa >= 0x40000000 && pa < 0x40000020)
        fprintf(stderr, "[wwatch] pa=0x%llx size=%u val=0x%llx pc=0x%llx\n",
                (unsigned long long)pa, size, (unsigned long long)value,
                (unsigned long long)m->cpu.cur_insn_pc);
    if (pa >= m->ram_base && pa + size <= m->ram_base + m->ram_size) {
        memcpy(m->ram + (pa - m->ram_base), &value, size);
        return;
    }
    if (pa >= m->flash_base && pa + size <= m->flash_base + m->flash_size) {
        if (m->flash_writable)
            memcpy(m->flash + (pa - m->flash_base), &value, size);
        return;
    }
    MMIODev *d = find_dev(m, pa);
    if (d && d->write) { d->write(d->opaque, pa - d->base, size, value); return; }

    m->last_bus_status = BUS_FAULT;
    if (g_trace)
        fprintf(stderr, "[bus] unmapped write pa=0x%llx size=%u val=0x%llx\n",
                (unsigned long long)pa, size, (unsigned long long)value);
}

void phys_write_blk(Machine *m, u64 pa, const void *src, u64 len) {
    const u8 *s = src;
    /* RAM fast path */
    if (pa >= m->ram_base && pa + len <= m->ram_base + m->ram_size) {
        memcpy(m->ram + (pa - m->ram_base), s, len);
        return;
    }
    if (pa >= m->flash_base && pa + len <= m->flash_base + m->flash_size) {
        memcpy(m->flash + (pa - m->flash_base), s, len);
        return;
    }
    for (u64 i = 0; i < len; i++) phys_write(m, pa + i, 1, s[i]);
}

void phys_read_blk(Machine *m, u64 pa, void *dst, u64 len) {
    u8 *d = dst;
    if (pa >= m->ram_base && pa + len <= m->ram_base + m->ram_size) {
        memcpy(d, m->ram + (pa - m->ram_base), len);
        return;
    }
    if (pa >= m->flash_base && pa + len <= m->flash_base + m->flash_size) {
        memcpy(d, m->flash + (pa - m->flash_base), len);
        return;
    }
    for (u64 i = 0; i < len; i++) d[i] = (u8)phys_read(m, pa + i, 1);
}
