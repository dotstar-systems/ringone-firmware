/*
 * Ring•One Firmware
 * Copyright (c) 2026 Dotstar Systems and Consulting (dotstarconsulting.com)
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include "ringone_sensors.h"
#include "ringone_ppg.h"

LOG_MODULE_REGISTER(ringone_sensors, LOG_LEVEL_INF);

/*
 * MAX30101 heart-rate / SpO2 sensor. `zephyr,deferred-init` in the
 * devicetree keeps the driver's POST_KERNEL init from running before the
 * sensor's power rail has settled — bring-up on nRF54LM20DK-A showed the
 * part NACKs I2C for a short window after boot. We soft-reset over raw
 * I2C and verify the part ID ourselves first (retrying, since that's the
 * transaction that actually fails while the rail is coming up), then
 * hand off to the driver's own device_init().
 */
#define MAX30101_REG_MODE_CFG  0x09
#define MAX30101_REG_PART_ID   0xFF
#define MAX30101_PART_ID_VAL   0x15
#define MAX30101_RESET_BIT     0x40

#define BRINGUP_MAX_ATTEMPTS   10
#define BRINGUP_RETRY_DELAY_MS 50

/* Sensor poll rate — matches the `smp-sr` (50 Hz) configured in
 * app.overlay. PPG post-processing needs one sample per conversion. */
#define PPG_POLL_PERIOD_MS     20
/* While the sensor isn't up yet, retry bring-up at a relaxed cadence. */
#define BRINGUP_RETRY_PERIOD_MS 1000

static const struct device *const max30101_dev =
	DEVICE_DT_GET(DT_ALIAS(heart_rate_sensor));

static const struct i2c_dt_spec max30101_i2c =
	I2C_DT_SPEC_GET(DT_ALIAS(heart_rate_sensor));

static struct k_work_delayable ppg_poll_work;

static int max30101_soft_reset(void)
{
	uint8_t mode_cfg;
	int ret;

	ret = i2c_reg_write_byte_dt(&max30101_i2c, MAX30101_REG_MODE_CFG,
				     MAX30101_RESET_BIT);
	if (ret) {
		return ret;
	}

	k_msleep(100);

	for (int i = 0; i < 50; i++) {
		ret = i2c_reg_read_byte_dt(&max30101_i2c, MAX30101_REG_MODE_CFG,
					    &mode_cfg);
		if (ret) {
			return ret;
		}
		if (!(mode_cfg & MAX30101_RESET_BIT)) {
			return 0;
		}
		k_msleep(10);
	}

	return -ETIMEDOUT;
}

static int max30101_verify_part_id(void)
{
	uint8_t part_id;
	int ret;

	ret = i2c_reg_read_byte_dt(&max30101_i2c, MAX30101_REG_PART_ID, &part_id);
	if (ret) {
		return ret;
	}

	if (part_id != MAX30101_PART_ID_VAL) {
		LOG_ERR("MAX30101 part ID mismatch: got 0x%02x, expected 0x%02x",
			part_id, MAX30101_PART_ID_VAL);
		return -EIO;
	}

	return 0;
}

static int max30101_bringup(void)
{
	int ret = -ETIMEDOUT;

	for (int attempt = 0; attempt < BRINGUP_MAX_ATTEMPTS; attempt++) {
		ret = max30101_soft_reset();
		if (!ret) {
			ret = max30101_verify_part_id();
			if (!ret) {
				break;
			}
		}
		k_msleep(BRINGUP_RETRY_DELAY_MS);
	}

	if (ret) {
		return ret;
	}

	ret = device_init(max30101_dev);
	if (ret) {
		LOG_ERR("MAX30101 device_init() failed (err %d)", ret);
		return ret;
	}

	LOG_INF("MAX30101 bring-up complete");
	ringone_ppg_reset();
	return 0;
}

static void ppg_poll_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!device_is_ready(max30101_dev)) {
		if (max30101_bringup()) {
			k_work_reschedule(&ppg_poll_work,
					  K_MSEC(BRINGUP_RETRY_PERIOD_MS));
			return;
		}
	}

	struct sensor_value red, ir;

	if (sensor_sample_fetch(max30101_dev) == 0 &&
	    sensor_channel_get(max30101_dev, SENSOR_CHAN_RED, &red) == 0 &&
	    sensor_channel_get(max30101_dev, SENSOR_CHAN_IR, &ir) == 0) {
		ringone_ppg_process_sample((uint32_t)red.val1, (uint32_t)ir.val1);
	} else {
		LOG_WRN("MAX30101 sample fetch failed");
	}

	k_work_reschedule(&ppg_poll_work, K_MSEC(PPG_POLL_PERIOD_MS));
}

int ringone_sensors_init(void)
{
	if (!device_is_ready(max30101_i2c.bus)) {
		LOG_ERR("MAX30101 I2C bus not ready");
		return -ENODEV;
	}

	ringone_ppg_reset();

	if (max30101_bringup()) {
		LOG_WRN("MAX30101 not responding yet — will keep retrying "
			"in the background; heart rate/SpO2 read as 0 until then");
	}

	k_work_init_delayable(&ppg_poll_work, ppg_poll_handler);
	k_work_schedule(&ppg_poll_work, K_MSEC(PPG_POLL_PERIOD_MS));

	LOG_INF("Ring-One sensors initialised");
	return 0;
}

/* RING_ONE_TODO: replace stub with real temperature driver
 * Suggested IC:  MAX30205
 * Interface:     I2C on &i2c20, addr 0x48
 * Zephyr driver: CONFIG_MAX30205=y
 */
int16_t ringone_read_temperature(void)
{
	return 2569; /* 25.69 °C */
}

/* Heart rate (BPM), post-processed from MAX30101 RED/IR samples.
 * Returns 0 while no finger is present / no valid reading yet. */
uint8_t ringone_read_heart_rate(void)
{
	return ringone_ppg_get_heart_rate();
}

/* SpO2 (%), post-processed from MAX30101 RED/IR samples.
 * Returns 0 while no finger is present / no valid reading yet. */
uint8_t ringone_read_spo2(void)
{
	return ringone_ppg_get_spo2();
}

/* RING_ONE_TODO: replace stub with real pedometer driver
 * Suggested IC:  LSM6DSO
 * Interface:     I2C on &i2c20, addr 0x6A
 * Zephyr driver: CONFIG_LSM6DSO=y
 */
uint32_t ringone_read_steps(void)
{
	static uint32_t count;

	return count++;
}

/* RING_ONE_TODO: replace stub with real battery measurement
 * Suggested IC:  nRF SAADC VBAT net
 * Interface:     ADC — sample VBAT voltage divider via SAADC channel
 * Zephyr driver: CONFIG_ADC=y
 */
uint8_t ringone_read_battery(void)
{
	return 85; /* % */
}
