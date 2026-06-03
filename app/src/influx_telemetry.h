/* Ring•One Firmware · Dotstar Consulting · Apache 2.0 */
#ifndef INFLUX_TELEMETRY_H
#define INFLUX_TELEMETRY_H

#include <stdbool.h>
#include "wifi_prov.h"   /* ringone_data_t */

/* Initialise PATH B — HTTPS → InfluxDB Cloud v3.
 * Spawns the "influx_pub" thread (stack 4096, priority 8).
 * Token is loaded at runtime from PSA Protected Storage; never in source. */
int influx_telemetry_init(void);

/* Queue a telemetry payload for HTTPS POST to InfluxDB.
 * Non-blocking — enqueues to the influx_pub thread's message queue.
 * Silently dropped if queue is full or Wi-Fi is not connected. */
void influx_telemetry_publish(const ringone_data_t *data);

/* True if the last InfluxDB POST returned HTTP 204. */
bool influx_telemetry_connected(void);

#endif /* INFLUX_TELEMETRY_H */
