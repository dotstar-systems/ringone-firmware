/*
 * Ring•One Firmware
 * Copyright (c) 2026 Dotstar Systems and Consulting (dotstarconsulting.com)
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Heart rate: peak-detect the IR channel's AC (pulsatile) component
 * against a dynamic, RMS-derived threshold, average the last few
 * inter-beat intervals, convert to BPM.
 *
 * SpO2: classic "ratio-of-ratios" over a rolling window —
 *   R = (RMS(AC_red)/DC_red) / (RMS(AC_ir)/DC_ir)
 *   SpO2 = 110 - 25*R  (linear approximation, Maxim AN6409-style)
 * This is an uncalibrated approximation adequate for bring-up; a
 * production release should calibrate the linear coefficients against
 * a reference pulse oximeter.
 */

#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "ringone_ppg.h"

#define SAMPLE_RATE_HZ      50
#define SAMPLE_PERIOD_MS    (1000 / SAMPLE_RATE_HZ)

#define DC_ALPHA            0.02f   /* DC tracker time constant ~1 s */
#define RMS_ALPHA           0.05f   /* AC RMS tracker time constant ~0.4 s */
#define PEAK_THRESHOLD_FRAC 0.3f    /* peak must exceed 30% of RMS(AC) */

#define MIN_IBI_MS          300     /* 200 BPM cap */
#define MAX_IBI_MS          2000    /* 30 BPM floor; longer gaps reset history */
#define IBI_HISTORY_LEN     4

#define MIN_AC_RMS_COUNTS   150.0f  /* below this: no finger / no pulse */
#define MIN_DC_COUNTS       1000.0f /* below this: no finger on sensor */

#define SPO2_WINDOW_SAMPLES (SAMPLE_RATE_HZ * 4) /* 4 s ratio window */

struct ppg_state {
	/* DC (baseline) trackers */
	float dc_red;
	float dc_ir;
	bool  dc_init;

	/* Heart-rate peak detection (driven off the IR channel) */
	float    ac_ir_prev;
	float    ac_ir_prev2;
	float    ac_ir_rms_sq_ema;
	uint32_t sample_idx;
	uint32_t last_beat_sample_idx;
	bool     have_last_beat;

	uint16_t ibi_history_ms[IBI_HISTORY_LEN];
	uint8_t  ibi_history_count;
	uint8_t  ibi_history_next;

	uint8_t  heart_rate_bpm;

	/* SpO2 ratio-of-ratios window accumulators */
	float    win_sum_ac_red_sq;
	float    win_sum_ac_ir_sq;
	float    win_sum_dc_red;
	float    win_sum_dc_ir;
	uint32_t win_count;

	uint8_t  spo2_pct;
};

static struct ppg_state s;

void ringone_ppg_reset(void)
{
	memset(&s, 0, sizeof(s));
}

static void update_heart_rate(float ac_ir)
{
	/* Track signal energy; used both as finger-presence gate and as the
	 * basis for a dynamic peak threshold that follows signal strength. */
	s.ac_ir_rms_sq_ema += (ac_ir * ac_ir - s.ac_ir_rms_sq_ema) * RMS_ALPHA;
	float ac_ir_rms = sqrtf(s.ac_ir_rms_sq_ema);

	bool signal_present = (ac_ir_rms >= MIN_AC_RMS_COUNTS) &&
			      (s.dc_ir >= MIN_DC_COUNTS);

	if (!signal_present) {
		/* No finger / no usable pulse: drop beat history so a stale
		 * reading doesn't linger once contact is lost. */
		s.have_last_beat = false;
		s.ibi_history_count = 0;
		s.heart_rate_bpm = 0;
		s.ac_ir_prev2 = s.ac_ir_prev;
		s.ac_ir_prev = ac_ir;
		s.sample_idx++;
		return;
	}

	float threshold = PEAK_THRESHOLD_FRAC * ac_ir_rms;

	/* Local maximum: previous sample is higher than both its neighbours
	 * and clears the dynamic threshold. */
	bool is_peak = (s.ac_ir_prev > s.ac_ir_prev2) &&
		       (s.ac_ir_prev >= ac_ir) &&
		       (s.ac_ir_prev > threshold);

	if (is_peak) {
		uint32_t peak_sample_idx = s.sample_idx - 1;

		if (!s.have_last_beat) {
			s.last_beat_sample_idx = peak_sample_idx;
			s.have_last_beat = true;
		} else {
			uint32_t ibi_ms = (peak_sample_idx - s.last_beat_sample_idx) *
					  SAMPLE_PERIOD_MS;

			if (ibi_ms < MIN_IBI_MS) {
				/* Refractory period — noise, ignore the peak
				 * entirely and keep waiting for the next one. */
			} else if (ibi_ms > MAX_IBI_MS) {
				/* Gap too long to average — resynchronise. */
				s.ibi_history_count = 0;
				s.ibi_history_next = 0;
				s.last_beat_sample_idx = peak_sample_idx;
			} else {
				s.ibi_history_ms[s.ibi_history_next] = (uint16_t)ibi_ms;
				s.ibi_history_next =
					(s.ibi_history_next + 1) % IBI_HISTORY_LEN;
				if (s.ibi_history_count < IBI_HISTORY_LEN) {
					s.ibi_history_count++;
				}
				s.last_beat_sample_idx = peak_sample_idx;

				uint32_t sum_ms = 0;

				for (int i = 0; i < s.ibi_history_count; i++) {
					sum_ms += s.ibi_history_ms[i];
				}
				uint32_t avg_ibi_ms = sum_ms / s.ibi_history_count;

				s.heart_rate_bpm = (uint8_t)(60000u / avg_ibi_ms);
			}
		}
	}

	s.ac_ir_prev2 = s.ac_ir_prev;
	s.ac_ir_prev = ac_ir;
	s.sample_idx++;
}

static void update_spo2_window(float ac_red, float ac_ir)
{
	s.win_sum_ac_red_sq += ac_red * ac_red;
	s.win_sum_ac_ir_sq  += ac_ir * ac_ir;
	s.win_sum_dc_red    += s.dc_red;
	s.win_sum_dc_ir     += s.dc_ir;
	s.win_count++;

	if (s.win_count < SPO2_WINDOW_SAMPLES) {
		return;
	}

	float rms_red    = sqrtf(s.win_sum_ac_red_sq / s.win_count);
	float rms_ir      = sqrtf(s.win_sum_ac_ir_sq / s.win_count);
	float mean_dc_red = s.win_sum_dc_red / s.win_count;
	float mean_dc_ir  = s.win_sum_dc_ir / s.win_count;

	if (mean_dc_red >= MIN_DC_COUNTS && mean_dc_ir >= MIN_DC_COUNTS &&
	    rms_ir >= MIN_AC_RMS_COUNTS) {
		float r = (rms_red / mean_dc_red) / (rms_ir / mean_dc_ir);
		float spo2 = 110.0f - 25.0f * r;

		if (spo2 > 100.0f) {
			spo2 = 100.0f;
		} else if (spo2 < 70.0f) {
			spo2 = 70.0f;
		}
		s.spo2_pct = (uint8_t)(spo2 + 0.5f);
	} else {
		s.spo2_pct = 0;
	}

	s.win_sum_ac_red_sq = 0.0f;
	s.win_sum_ac_ir_sq  = 0.0f;
	s.win_sum_dc_red    = 0.0f;
	s.win_sum_dc_ir     = 0.0f;
	s.win_count         = 0;
}

void ringone_ppg_process_sample(uint32_t red_raw, uint32_t ir_raw)
{
	if (!s.dc_init) {
		s.dc_red  = (float)red_raw;
		s.dc_ir   = (float)ir_raw;
		s.dc_init = true;
	} else {
		s.dc_red += ((float)red_raw - s.dc_red) * DC_ALPHA;
		s.dc_ir  += ((float)ir_raw - s.dc_ir) * DC_ALPHA;
	}

	float ac_red = (float)red_raw - s.dc_red;
	float ac_ir  = (float)ir_raw - s.dc_ir;

	update_heart_rate(ac_ir);
	update_spo2_window(ac_red, ac_ir);
}

uint8_t ringone_ppg_get_heart_rate(void)
{
	return s.heart_rate_bpm;
}

uint8_t ringone_ppg_get_spo2(void)
{
	return s.spo2_pct;
}
