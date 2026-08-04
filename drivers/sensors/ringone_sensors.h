/*
 * Ring•One Firmware
 * Copyright (c) 2026 Dotstar Systems (dotstarsystems.com)
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RINGONE_SENSORS_H_
#define RINGONE_SENSORS_H_

#include <stdint.h>

int      ringone_sensors_init(void);
int16_t  ringone_read_temperature(void);
uint8_t  ringone_read_heart_rate(void);
uint8_t  ringone_read_spo2(void);
uint32_t ringone_read_steps(void);
uint8_t  ringone_read_battery(void);

/* Manually fetch one raw MAX30101 RED/IR sample (e.g. for the
 * `ringone spo2` shell command). Returns -ENODEV if the sensor isn't
 * up yet, -EIO on an I2C fetch error. */
int ringone_sensors_read_raw(uint32_t *red, uint32_t *ir);

#endif /* RINGONE_SENSORS_H_ */
