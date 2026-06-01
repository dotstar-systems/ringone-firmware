/*
 * Ring•One Firmware
 * Copyright (c) 2026 Dotstar Systems and Consulting (dotstarconsulting.com)
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include "ringone_sensors.h"

LOG_MODULE_REGISTER(ringone_sensors, LOG_LEVEL_INF);

int ringone_sensors_init(void)
{
	LOG_INF("Ring-One sensors initialised (stub mode)");
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

/* RING_ONE_TODO: replace stub with real heart rate driver
 * Suggested IC:  MAX30101
 * Interface:     I2C on &i2c20, addr 0x57
 * Zephyr driver: CONFIG_MAX30101=y
 */
uint8_t ringone_read_heart_rate(void)
{
	return 72; /* BPM */
}

/* RING_ONE_TODO: replace stub with real SpO2 driver
 * Suggested IC:  MAX30101
 * Interface:     I2C on &i2c20, addr 0x57
 * Zephyr driver: CONFIG_MAX30101=y
 */
uint8_t ringone_read_spo2(void)
{
	return 98; /* % */
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
