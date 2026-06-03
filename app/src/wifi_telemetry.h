/* Ring•One Firmware · Dotstar Consulting · Apache 2.0 */
#ifndef WIFI_TELEMETRY_H
#define WIFI_TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>
#include "ringone_sensors.h"

/*
 * Ring•One telemetry payload — maps 1:1 to MQTT JSON and
 * InfluxDB field keys. Keep field names stable — changing
 * them breaks the Grafana dashboard queries.
 */
typedef struct {
	uint32_t timestamp;     /* unix seconds (from SNTP) */
	int16_t  temperature;   /* 0.01°C per LSB */
	uint8_t  heart_rate;    /* BPM */
	uint8_t  spo2;          /* % */
	uint32_t steps;         /* count since boot */
	uint8_t  battery;       /* % */
	int8_t   rssi_ble;      /* dBm — last BLE connection RSSI */
} ringone_telemetry_t;

/* Initialise Wi-Fi stack and connect using stored credentials.
 * Call once from main after bt_enable(). Returns 0 on success. */
int wifi_telemetry_init(void);

/* Publish one telemetry payload to MQTT broker.
 * Call from the sensor loop every TELEMETRY_INTERVAL_SEC seconds.
 * Non-blocking — returns immediately if Wi-Fi is not connected.
 * Returns 0 on success, negative errno on failure. */
int wifi_telemetry_publish(const ringone_telemetry_t *payload);

/* True if Wi-Fi is connected and MQTT session is live */
bool wifi_telemetry_connected(void);

/* Write MQTT username and password into PSA Protected Storage.
 * Credentials are encrypted with a key derived from the CRACEN hardware
 * unique key — device-unique, not readable from software.
 * Call this once from a factory provisioning tool or secure shell command.
 * wifi_telemetry_init() will use these on all subsequent boots instead of
 * the Kconfig defaults. Returns 0 on success, negative errno on failure. */
int wifi_telemetry_provision_mqtt_creds(const char *username,
					const char *password);

#endif /* WIFI_TELEMETRY_H */
