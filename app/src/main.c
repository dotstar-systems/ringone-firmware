/* Ring•One Firmware · Dotstar Systems · Apache 2.0 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <hw_unique_key.h>
#include "ringone_sensors.h"
#include "wifi_prov.h"
#include "influx_telemetry.h"
#include "mqtt_client.h"
#include "sntp_sync.h"
#include "watchdog.h"

LOG_MODULE_REGISTER(ringone_main, LOG_LEVEL_INF);

/* ── UUIDs ────────────────────────────────────────────────────────── */
#define UUID_SVC_VAL      BT_UUID_128_ENCODE(0xfd0d5c94, 0x193c, 0x496e, \
					     0xb80f, 0x511a474a449f)
#define UUID_TEMP_VAL     BT_UUID_128_ENCODE(0xac70a713, 0x348e, 0x43db, \
					     0xbf84, 0xffce9d82120d)
#define UUID_HR_VAL       BT_UUID_128_ENCODE(0x75fb4a26, 0x440c, 0x4dd3, \
					     0xbe96, 0x91ad75ecb864)
#define UUID_SPO2_VAL     BT_UUID_128_ENCODE(0xc4671ec2, 0x35f1, 0x40c4, \
					     0x887b, 0x37bc00ec3427)
#define UUID_STEP_VAL     BT_UUID_128_ENCODE(0x7956ed3f, 0x1cb4, 0x47ce, \
					     0x89ad, 0x9742bc0ab8bf)
#define UUID_BAT_VAL      BT_UUID_128_ENCODE(0x02e35db9, 0x662d, 0x4229, \
					     0xa874, 0xd4f04c82653a)

/* Wi-Fi provisioning characteristics */
#define UUID_WIFI_SSID_VAL  BT_UUID_128_ENCODE(0xa1b2c3d4, 0x0001, 0x0001, \
					       0x0001, 0xa1b2c3d40001)
#define UUID_WIFI_PASS_VAL  BT_UUID_128_ENCODE(0xa1b2c3d4, 0x0001, 0x0001, \
					       0x0001, 0xa1b2c3d40002)
#define UUID_WIFI_STAT_VAL  BT_UUID_128_ENCODE(0xa1b2c3d4, 0x0001, 0x0001, \
					       0x0001, 0xa1b2c3d40003)

#define UUID_SVC       BT_UUID_DECLARE_128(UUID_SVC_VAL)
#define UUID_TEMP      BT_UUID_DECLARE_128(UUID_TEMP_VAL)
#define UUID_HR        BT_UUID_DECLARE_128(UUID_HR_VAL)
#define UUID_SPO2      BT_UUID_DECLARE_128(UUID_SPO2_VAL)
#define UUID_STEP      BT_UUID_DECLARE_128(UUID_STEP_VAL)
#define UUID_BAT       BT_UUID_DECLARE_128(UUID_BAT_VAL)
#define UUID_WIFI_SSID BT_UUID_DECLARE_128(UUID_WIFI_SSID_VAL)
#define UUID_WIFI_PASS BT_UUID_DECLARE_128(UUID_WIFI_PASS_VAL)
#define UUID_WIFI_STAT BT_UUID_DECLARE_128(UUID_WIFI_STAT_VAL)

/* ── Shared sensor data ───────────────────────────────────────────── */
static ringone_data_t g_data;

/* ── GATT ─────────────────────────────────────────────────────────── */
static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(value);
}

/*
 * Attribute index map (BT_GATT_CHARACTERISTIC expands to 2 entries):
 *   [0]  primary service declaration
 *   [1]  temperature char declaration
 *   [2]  temperature char value      ← notify  (unchanged)
 *   [3]  temperature CCC
 *   [4]  heart_rate char declaration
 *   [5]  heart_rate char value       ← notify  (unchanged)
 *   [6]  heart_rate CCC
 *   [7]  spo2 char declaration
 *   [8]  spo2 char value             ← notify  (unchanged)
 *   [9]  spo2 CCC
 *   [10] steps char declaration
 *   [11] steps char value            ← notify  (unchanged)
 *   [12] steps CCC
 *   [13] battery char declaration
 *   [14] battery char value          ← notify  (unchanged)
 *   [15] battery CCC
 *   [16] wifi_ssid char declaration
 *   [17] wifi_ssid char value        ← write
 *   [18] wifi_pass char declaration
 *   [19] wifi_pass char value        ← write (ENCRYPT, requires bond)
 *   [20] wifi_status char declaration
 *   [21] wifi_status char value      ← notify + read
 *   [22] wifi_status CCC
 */

/* Write callbacks — delegate to wifi_prov.c */
static ssize_t wifi_ssid_write(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len,
				uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
	wifi_prov_on_ssid_write(buf, len);
	return (ssize_t)len;
}

static ssize_t wifi_pass_write(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len,
				uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
	wifi_prov_on_password_write(buf, len);
	return (ssize_t)len;
}

static ssize_t wifi_status_read(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	uint8_t status = (uint8_t)wifi_prov_get_status();

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &status, sizeof(status));
}

BT_GATT_SERVICE_DEFINE(ringone_svc,
	BT_GATT_PRIMARY_SERVICE(UUID_SVC),

	/* Temperature — unchanged */
	BT_GATT_CHARACTERISTIC(UUID_TEMP, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* Heart Rate — unchanged */
	BT_GATT_CHARACTERISTIC(UUID_HR, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* SpO2 — unchanged */
	BT_GATT_CHARACTERISTIC(UUID_SPO2, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* Steps — unchanged */
	BT_GATT_CHARACTERISTIC(UUID_STEP, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* Battery — unchanged */
	BT_GATT_CHARACTERISTIC(UUID_BAT, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* WiFi SSID — write only, no encryption required */
	BT_GATT_CHARACTERISTIC(UUID_WIFI_SSID,
		BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
		BT_GATT_PERM_WRITE,
		NULL, wifi_ssid_write, NULL),

	/* WiFi Password — write requires bonded connection (WRITE_ENCRYPT) */
	BT_GATT_CHARACTERISTIC(UUID_WIFI_PASS,
		BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
		BT_GATT_PERM_WRITE_ENCRYPT,
		NULL, wifi_pass_write, NULL),

	/* WiFi Status — notify + read, value: 0x00–0x03 */
	BT_GATT_CHARACTERISTIC(UUID_WIFI_STAT,
		BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ,
		wifi_status_read, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* ── Advertising ──────────────────────────────────────────────────── */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, UUID_SVC_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE,
		CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void start_advertising(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("Advertising start failed (err %d)", err);
	}
}

static void adv_restart_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart_work_handler);

static void adv_restart_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	start_advertising();
}

/* ── Connection callbacks ─────────────────────────────────────────── */
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("Connection failed (err %u)", err);
		return;
	}
	LOG_INF("Ring-One connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("Ring-One disconnected (reason %u)", reason);
	k_work_schedule(&adv_restart_work, K_MSEC(100));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected,
	.disconnected = disconnected,
};

/* ── Notify + telemetry loop (2 s period) ─────────────────────────── */
static void notify_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(notify_work, notify_work_handler);

static uint32_t s_notify_ticks;

/* ── InfluxDB publish loop (own cadence, RINGONE_INFLUX_INTERVAL_SEC) ──
 * Decoupled from notify_work's 2 s sensor-read cadence: this just reposts
 * whatever g_data last held. g_data is only ever written by notify_work_handler
 * and only ever read here, and both run on the system workqueue, so there's
 * no concurrent access despite the two work items being logically separate. */
static void influx_post_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(influx_post_work, influx_post_work_handler);

static void influx_post_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	influx_telemetry_publish(&g_data);   /* PATH B */
	k_work_reschedule(&influx_post_work,
			  K_SECONDS(CONFIG_RINGONE_INFLUX_INTERVAL_SEC));
}

static void notify_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Read sensors */
	g_data.temperature = ringone_read_temperature();
	g_data.heart_rate  = ringone_read_heart_rate();
	g_data.spo2        = ringone_read_spo2();
	g_data.steps       = ringone_read_steps();
	g_data.battery     = ringone_read_battery();

	/* BLE notify — existing characteristics unchanged at attrs[2,5,8,11,14] */
	bt_gatt_notify(NULL, &ringone_svc.attrs[2],  &g_data.temperature,
		       sizeof(g_data.temperature));
	bt_gatt_notify(NULL, &ringone_svc.attrs[5],  &g_data.heart_rate,
		       sizeof(g_data.heart_rate));
	bt_gatt_notify(NULL, &ringone_svc.attrs[8],  &g_data.spo2,
		       sizeof(g_data.spo2));
	bt_gatt_notify(NULL, &ringone_svc.attrs[11], &g_data.steps,
		       sizeof(g_data.steps));
	bt_gatt_notify(NULL, &ringone_svc.attrs[14], &g_data.battery,
		       sizeof(g_data.battery));

	/* Feed hardware watchdog every 2 s tick (well within 30 s window) */
	watchdog_feed();

	/* MQTT telemetry counter: publish every RINGONE_TELEMETRY_INTERVAL_SEC.
	 * InfluxDB (PATH B) runs on its own faster, independent cadence — see
	 * influx_post_work below — since it reuses a persistent connection and
	 * isn't tied to MQTT's publish-cost tradeoff. */
	if (IS_ENABLED(CONFIG_RINGONE_CLOUD_ENABLE)) {
		s_notify_ticks += 2;
		if (s_notify_ticks >= CONFIG_RINGONE_TELEMETRY_INTERVAL_SEC) {
			s_notify_ticks = 0;
			mqtt_publish_telemetry(&g_data);     /* PATH C */
		}
	}

	int16_t temp_int  = g_data.temperature / 100;
	int16_t temp_frac = g_data.temperature % 100;

	if (temp_frac < 0) {
		temp_frac = -temp_frac;
	}

	LOG_INF("[%u] temp=%d.%02d hr=%u spo2=%u steps=%u bat=%u "
		"wifi=%s influx=%s mqtt=%s",
		sntp_get_unix_time(),
		(int)temp_int, (int)temp_frac,
		g_data.heart_rate, g_data.spo2,
		g_data.steps, g_data.battery,
		wifi_prov_get_status() == WIFI_STATUS_CONNECTED
			? "UP" : "DOWN",
		influx_telemetry_connected() ? "OK" : "--",
		mqtt_client_connected()      ? "OK" : "--");

	k_work_reschedule(&notify_work, K_SECONDS(2));
}

/* ── Entry point ──────────────────────────────────────────────────── */
int main(void)
{
	int err;

	LOG_INF("Ring-One | Dotstar Systems | "
		"dotstarsystems.com");

	/* a. Hardware watchdog — must be first */
	err = watchdog_init();
	if (err) {
		LOG_WRN("watchdog_init failed (err %d) — continuing", err);
	}

	/* PSA Protected Storage (ringone_cred, TLS credential backend) derives
	 * its AEAD key from a CRACEN hardware-unique key held in KMU. A
	 * never-provisioned chip has none, and every psa_ps_*() call fails
	 * with PSA_ERROR_BAD_STATE until one is written — do that here, once,
	 * before anything touches protected storage. */
	if (!hw_unique_key_are_any_written()) {
		int huk_err = hw_unique_key_write_random();

		if (huk_err != HW_UNIQUE_KEY_SUCCESS) {
			LOG_ERR("hw_unique_key_write_random failed (err %d)", huk_err);
		} else {
			LOG_INF("Provisioned random hardware-unique key into KMU");
		}
	}

	err = ringone_sensors_init();
	if (err) {
		LOG_ERR("Sensor init failed (err %d)", err);
		return err;
	}

	/* b. BLE + GATT server */
	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}

	/* Load BT identity address and bonding keys from NVS before advertising.
	 * Required when CONFIG_BT_SETTINGS=y — without this the controller has
	 * no ID address and bt_le_adv_start() returns -EAGAIN. */
	settings_load();

	/* Pass WiFi-Status GATT attr to wifi_prov for notify (attrs[21]) */
	wifi_prov_set_status_attr(&ringone_svc.attrs[21]);

	/* Start advertising before Wi-Fi init so the ring is discoverable
	 * immediately — provisioning may block on first boot. */
	start_advertising();
	LOG_INF("Ring-One advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);

	if (IS_ENABLED(CONFIG_RINGONE_CLOUD_ENABLE)) {
		/* c. Wi-Fi provisioning — async, spawns wifi_prov thread */
		err = wifi_prov_init();
		if (err) {
			LOG_ERR("wifi_prov_init failed (err %d)", err);
		}

		/* d. SNTP time sync — async, spawns sntp_sync thread */
		sntp_sync();

		/* e. InfluxDB HTTPS telemetry — async, spawns influx_pub thread */
		err = influx_telemetry_init();
		if (err) {
			LOG_ERR("influx_telemetry_init failed (err %d)", err);
		}
		k_work_schedule(&influx_post_work,
				K_SECONDS(CONFIG_RINGONE_INFLUX_INTERVAL_SEC));

		/* f. MQTT HiveMQ bidirectional — async, spawns mqtt_client thread */
		err = ringone_mqtt_init();
		if (err) {
			LOG_ERR("mqtt_client_init failed (err %d)", err);
		}
	} else {
		LOG_WRN("RINGONE_CLOUD_ENABLE=n — BLE-only mode, "
			"Wi-Fi/InfluxDB/MQTT skipped");
	}

	/* Start 2 s BLE notify + telemetry loop */
	k_work_schedule(&notify_work, K_SECONDS(2));

	return 0;
}
