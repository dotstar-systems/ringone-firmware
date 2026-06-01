/*
 * Ring•One Firmware
 * Copyright (c) 2026 Dotstar Systems and Consulting (dotstarconsulting.com)
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

#endif /* RINGONE_SENSORS_H_ */
