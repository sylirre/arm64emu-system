/* Raw terminal handling for the serial console (host stdin/stdout). */
#ifndef A64_TTY_H
#define A64_TTY_H

#include "types.h"

/* Put the controlling terminal into raw mode (no echo/canon, non-blocking
 * stdin). Safe to call when stdin is not a TTY (no-op). Restored on exit. */
void tty_raw_enable(void);
void tty_raw_disable(void);

/* Non-blocking read of one byte from stdin. Returns -1 if none available. */
int tty_getchar(void);

/* Write one byte to stdout (the emulated UART TX). */
void tty_putchar(int ch);

#endif /* A64_TTY_H */
