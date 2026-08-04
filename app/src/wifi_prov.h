/* Ring•One Firmware · Dotstar Systems · Apache 2.0 */
#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/bluetooth/gatt.h>

/* ── Shared sensor telemetry payload ────────────────────────────────
 * Used by influx_telemetry and mqtt_client.  Keep field names stable —
 * changing them breaks Grafana dashboard queries. */
typedef struct {
	int16_t  temperature;   /* 0.01 °C per LSB */
	uint8_t  heart_rate;    /* BPM */
	uint8_t  spo2;          /* % */
	uint32_t steps;         /* count since boot */
	uint8_t  battery;       /* % */
} ringone_data_t;

/* ── Wi-Fi connection status ────────────────────────────────────────
 * Matches the GATT WiFi-Status characteristic values. */
typedef enum {
	WIFI_STATUS_NOT_PROVISIONED = 0x00,
	WIFI_STATUS_CONNECTING      = 0x01,
	WIFI_STATUS_CONNECTED       = 0x02,
	WIFI_STATUS_FAILED          = 0x03,
} ringone_wifi_status_t;

/* ── Public API ─────────────────────────────────────────────────────*/

/* Check NVS for stored credentials; connect immediately if found.
 * If not found, idle until BLE write or button long-press triggers
 * SoftAP mode.  Spawns the "wifi_prov" thread (stack 4096, priority 7).
 * Call after bt_enable(). */
int wifi_prov_init(void);

/* Return current Wi-Fi connection state. */
ringone_wifi_status_t wifi_prov_get_status(void);

/* Called from GATT write callbacks in main.c when the companion app
 * writes the WiFi-SSID characteristic. */
void wifi_prov_on_ssid_write(const uint8_t *data, uint16_t len);

/* Called from GATT write callbacks in main.c when the companion app
 * writes the WiFi-Password characteristic (requires bonded connection). */
void wifi_prov_on_password_write(const uint8_t *data, uint16_t len);

/* Pass the GATT attribute pointer for WiFi-Status so wifi_prov.c can
 * call bt_gatt_notify() on status changes.  Call once after bt_enable()
 * with &ringone_svc.attrs[<status_value_idx>]. */
void wifi_prov_set_status_attr(const struct bt_gatt_attr *attr);

#endif /* WIFI_PROV_H */
