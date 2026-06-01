/*
 * Ring•One Firmware
 * Copyright (c) 2026 Dotstar Systems and Consulting (dotstarconsulting.com)
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include "ringone_sensors.h"

LOG_MODULE_REGISTER(ringone_main, LOG_LEVEL_INF);

/* ── UUIDs ────────────────────────────────────────────────────────── */
#define UUID_SVC_VAL  BT_UUID_128_ENCODE(0xfd0d5c94, 0x193c, 0x496e, 0xb80f, 0x511a474a449f)
#define UUID_TEMP_VAL BT_UUID_128_ENCODE(0xac70a713, 0x348e, 0x43db, 0xbf84, 0xffce9d82120d)
#define UUID_HR_VAL   BT_UUID_128_ENCODE(0x75fb4a26, 0x440c, 0x4dd3, 0xbe96, 0x91ad75ecb864)
#define UUID_SPO2_VAL BT_UUID_128_ENCODE(0xc4671ec2, 0x35f1, 0x40c4, 0x887b, 0x37bc00ec3427)
#define UUID_STEP_VAL BT_UUID_128_ENCODE(0x7956ed3f, 0x1cb4, 0x47ce, 0x89ad, 0x9742bc0ab8bf)
#define UUID_BAT_VAL  BT_UUID_128_ENCODE(0x02e35db9, 0x662d, 0x4229, 0xa874, 0xd4f04c82653a)

#define UUID_SVC  BT_UUID_DECLARE_128(UUID_SVC_VAL)
#define UUID_TEMP BT_UUID_DECLARE_128(UUID_TEMP_VAL)
#define UUID_HR   BT_UUID_DECLARE_128(UUID_HR_VAL)
#define UUID_SPO2 BT_UUID_DECLARE_128(UUID_SPO2_VAL)
#define UUID_STEP BT_UUID_DECLARE_128(UUID_STEP_VAL)
#define UUID_BAT  BT_UUID_DECLARE_128(UUID_BAT_VAL)

/* ── Data ─────────────────────────────────────────────────────────── */
typedef struct {
	int16_t  temperature;  /* 0.01 °C per LSB */
	uint8_t  heart_rate;   /* BPM */
	uint8_t  spo2;         /* % */
	uint32_t steps;        /* count since boot */
	uint8_t  battery;      /* % */
} ringone_data_t;

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
 *   [2]  temperature char value      ← notify
 *   [3]  temperature CCC
 *   [4]  heart_rate char declaration
 *   [5]  heart_rate char value       ← notify
 *   [6]  heart_rate CCC
 *   [7]  spo2 char declaration
 *   [8]  spo2 char value             ← notify
 *   [9]  spo2 CCC
 *   [10] steps char declaration
 *   [11] steps char value            ← notify
 *   [12] steps CCC
 *   [13] battery char declaration
 *   [14] battery char value          ← notify
 *   [15] battery CCC
 */
BT_GATT_SERVICE_DEFINE(ringone_svc,
	BT_GATT_PRIMARY_SERVICE(UUID_SVC),

	BT_GATT_CHARACTERISTIC(UUID_TEMP, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(UUID_HR, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(UUID_SPO2, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(UUID_STEP, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(UUID_BAT, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
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
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("Advertising start failed (err %d)", err);
	}
}

/* ── Advertising restart work (deferred to let controller settle) ─── */
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
	/* Controller needs ~100 ms to release resources before accepting a new
	 * adv_start; calling it synchronously returns -EBUSY. */
	k_work_schedule(&adv_restart_work, K_MSEC(100));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected,
	.disconnected = disconnected,
};

/* ── Notify loop (2 s period) ─────────────────────────────────────── */
static void notify_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(notify_work, notify_work_handler);

static void notify_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	g_data.temperature = ringone_read_temperature();
	g_data.heart_rate  = ringone_read_heart_rate();
	g_data.spo2        = ringone_read_spo2();
	g_data.steps       = ringone_read_steps();
	g_data.battery     = ringone_read_battery();

	bt_gatt_notify(NULL, &ringone_svc.attrs[2],  &g_data.temperature, sizeof(g_data.temperature));
	bt_gatt_notify(NULL, &ringone_svc.attrs[5],  &g_data.heart_rate,  sizeof(g_data.heart_rate));
	bt_gatt_notify(NULL, &ringone_svc.attrs[8],  &g_data.spo2,        sizeof(g_data.spo2));
	bt_gatt_notify(NULL, &ringone_svc.attrs[11], &g_data.steps,       sizeof(g_data.steps));
	bt_gatt_notify(NULL, &ringone_svc.attrs[14], &g_data.battery,     sizeof(g_data.battery));

	int16_t temp_int  = g_data.temperature / 100;
	int16_t temp_frac = g_data.temperature % 100;

	if (temp_frac < 0) {
		temp_frac = -temp_frac;
	}

	LOG_INF("temp=%d.%02d hr=%u spo2=%u steps=%u bat=%u",
		(int)temp_int, (int)temp_frac,
		g_data.heart_rate, g_data.spo2, g_data.steps, g_data.battery);

	k_work_reschedule(&notify_work, K_SECONDS(2));
}

/* ── Entry point ──────────────────────────────────────────────────── */
int main(void)
{
	int err;

	LOG_INF("Ring-One | Dotstar Systems and Consulting | dotstarconsulting.com");

	err = ringone_sensors_init();
	if (err) {
		LOG_ERR("Sensor init failed (err %d)", err);
		return err;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}

	start_advertising();
	LOG_INF("Ring-One advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);

	k_work_schedule(&notify_work, K_SECONDS(2));

	return 0;
}
