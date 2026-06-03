/* Ring•One Firmware · Dotstar Consulting · Apache 2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/settings/settings.h>
#include <string.h>

#include "watchdog.h"

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_INF);

#define WDT_TIMEOUT_MS  30000U   /* 30 s hardware watchdog */

/* Use the board-defined watchdog0 alias (maps to wdt31 on nRF54LM20A).
 * The node must be enabled via DTS overlay — see app.overlay. */
static const struct device *const s_wdt =
	DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int s_wdt_channel = -1;

#define CRASH_COUNT_KEY "ringone/crash_count"
static uint32_t s_crash_count;

static int crash_count_load(const char *key, size_t len,
			    settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(key, "crash_count") == 0 && len == sizeof(s_crash_count)) {
		read_cb(cb_arg, &s_crash_count, sizeof(s_crash_count));
	}
	return 0;
}

static struct settings_handler s_crash_handler = {
	.name    = "ringone",
	.h_get   = NULL,
	.h_set   = crash_count_load,
	.h_commit = NULL,
	.h_export = NULL,
};

int watchdog_init(void)
{
	int err;

	/* ── Check for watchdog reset at previous boot ── */
	uint32_t cause = 0;

	hwinfo_get_reset_cause(&cause);
	hwinfo_clear_reset_cause();

	if (cause & RESET_WATCHDOG) {
		/* Load crash count, increment, save */
		settings_subsys_init();
		settings_register(&s_crash_handler);
		settings_load();

		s_crash_count++;
		settings_save_one(CRASH_COUNT_KEY, &s_crash_count,
				  sizeof(s_crash_count));

		LOG_ERR("WATCHDOG RESET DETECTED — previous boot was watchdog "
			"(crash #%u)", s_crash_count);
	}

	/* ── RING_ONE_TODO: add fault handlers (HardFault, MemManage) ──
	 * that write crash reason to NVS before triggering reset.
	 * Use CMSE fault handler hooks via zephyr/arch/arm/core/fatal.c
	 * override or CONFIG_CORTEX_M_DEBUG=y + custom k_sys_fatal_error_handler. */

	if (!device_is_ready(s_wdt)) {
		LOG_ERR("Watchdog device %s not ready", s_wdt->name);
		return -ENODEV;
	}

	struct wdt_timeout_cfg wdt_cfg = {
		.window = {
			.min = 0,
			.max = WDT_TIMEOUT_MS,
		},
		.callback  = NULL,  /* trigger immediate reset on expiry */
		.flags     = WDT_FLAG_RESET_SOC,
	};

	s_wdt_channel = wdt_install_timeout(s_wdt, &wdt_cfg);
	if (s_wdt_channel < 0) {
		LOG_ERR("wdt_install_timeout failed (err %d)", s_wdt_channel);
		return s_wdt_channel;
	}

	err = wdt_setup(s_wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err) {
		LOG_ERR("wdt_setup failed (err %d)", err);
		return err;
	}

	LOG_INF("Watchdog armed: %u ms timeout, channel %d",
		WDT_TIMEOUT_MS, s_wdt_channel);
	return 0;
}

void watchdog_feed(void)
{
	if (s_wdt_channel >= 0) {
		wdt_feed(s_wdt, s_wdt_channel);
	}
}

uint32_t watchdog_get_crash_count(void)
{
	return s_crash_count;
}
