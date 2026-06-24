/* PL031 real-time clock (minimal: returns host wall-clock seconds). */
#include "../devices.h"
#include <stdlib.h>
#include <time.h>

static const u8 pl031_id[8] = { 0x31, 0x10, 0x14, 0x00, 0x0d, 0xf0, 0x05, 0xb1 };

static u64 rtc_read(void *opaque, u64 off, unsigned size) {
    PL031 *p = opaque;
    if (off >= 0xfe0 && off <= 0xffc) return pl031_id[(off - 0xfe0) / 4];
    switch (off) {
        case 0x00: return (u32)time(NULL) + p->lr;   /* DR */
        case 0x04: return p->mr;
        case 0x08: return p->lr;
        case 0x0c: return p->cr;
        case 0x10: return p->imsc;
        case 0x14: return p->ris;
        case 0x18: return p->ris & p->imsc;
        default: return 0;
    }
}

static void rtc_write(void *opaque, u64 off, unsigned size, u64 val) {
    PL031 *p = opaque;
    switch (off) {
        case 0x04: p->mr = (u32)val; break;
        case 0x08: p->lr = (u32)val; break;
        case 0x0c: p->cr = (u32)val; break;
        case 0x10: p->imsc = (u32)val; break;
        case 0x1c: p->ris &= ~(u32)val; break;
        default: break;
    }
}

PL031 *pl031_create(Machine *m, GIC *gic) {
    PL031 *p = calloc(1, sizeof(*p));
    p->gic = gic;
    machine_add_device(m, RTC_BASE, 0x1000, rtc_read, rtc_write, p, "pl031");
    m->rtc = p;
    return p;
}
