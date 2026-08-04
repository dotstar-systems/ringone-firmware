/* Ring•One Firmware · Dotstar Systems · Apache 2.0 */
#ifndef SNTP_SYNC_H
#define SNTP_SYNC_H

#include <stdint.h>

/* Start async SNTP sync.  Returns immediately; sync runs in background.
 * Call once after network is up.  Re-called by retry logic on failure. */
void sntp_sync(void);

/* Return current Unix timestamp (UTC seconds).
 * Returns an uptime-based estimate if SNTP has not yet synced. */
uint32_t sntp_get_unix_time(void);

#endif /* SNTP_SYNC_H */
