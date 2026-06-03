/* Ring•One Firmware · Dotstar Consulting · Apache 2.0 */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdbool.h>
#include "wifi_prov.h"   /* ringone_data_t */

/* Initialise PATH C — MQTT/TLS → HiveMQ Cloud.
 * Spawns the "mqtt_client" thread (stack 4096, priority 9).
 * Credentials loaded from PSA Protected Storage (falls back to Kconfig). */
int ringone_mqtt_init(void);

/* Publish telemetry JSON to ring-one/<device_id>/telemetry (QoS 0).
 * Called from the main notify loop every RINGONE_TELEMETRY_INTERVAL_SEC.
 * Non-blocking — returns immediately if MQTT is not connected. */
void mqtt_publish_telemetry(const ringone_data_t *data);

/* True if the MQTT session with HiveMQ is live. */
bool mqtt_client_connected(void);

#endif /* MQTT_CLIENT_H */
