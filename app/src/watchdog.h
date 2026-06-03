/* Ring•One Firmware · Dotstar Consulting · Apache 2.0 */
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

/* Initialise hardware watchdog (30 s timeout).
 * Logs "WATCHDOG RESET DETECTED" if previous boot was a WDT reset.
 * Call before any other subsystem init. */
int watchdog_init(void);

/* Feed the watchdog.  Call from the main loop every 10 s. */
void watchdog_feed(void);

/* Return watchdog reset counter (persisted across reboots in Settings). */
uint32_t watchdog_get_crash_count(void);

#endif /* WATCHDOG_H */
