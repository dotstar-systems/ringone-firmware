/*
 * Ring•One Firmware
 * Copyright (c) 2026 Dotstar Systems and Consulting (dotstarconsulting.com)
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RINGONE_PPG_H_
#define RINGONE_PPG_H_

#include <stdint.h>

/* PPG (photoplethysmography) post-processing: turns raw MAX30101 RED/IR
 * ADC counts into heart rate (BPM) and SpO2 (%). Feed it every sample at
 * the sensor's configured rate (see ringone_sensors.c, ~50 Hz). */

/* Reset all running state (DC trackers, beat history, window accumulators).
 * Call once after the sensor is (re-)initialised. */
void ringone_ppg_reset(void);

/* Feed one RED+IR sample pair. */
void ringone_ppg_process_sample(uint32_t red_raw, uint32_t ir_raw);

/* Latest heart rate in BPM, or 0 if no valid reading yet (no finger
 * present / insufficient signal). */
uint8_t ringone_ppg_get_heart_rate(void);

/* Latest SpO2 in %, or 0 if no valid reading yet. */
uint8_t ringone_ppg_get_spo2(void);

#endif /* RINGONE_PPG_H_ */
