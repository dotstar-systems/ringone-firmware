/* Ring•One Firmware · Dotstar Systems · Apache 2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/sntp.h>

#include "sntp_sync.h"

LOG_MODULE_REGISTER(sntp_sync, LOG_LEVEL_INF);

#define SNTP_SERVER      "pool.ntp.org"
#define SNTP_TIMEOUT_MS  5000
#define SNTP_RETRY_SEC   30
#define SNTP_MAX_RETRIES 5

static K_THREAD_STACK_DEFINE(s_sntp_stack, 2048);
static struct k_thread s_sntp_thread;

static struct net_mgmt_event_callback s_l4_cb;
static K_SEM_DEFINE(s_net_sem, 0, 1);

/* Unix timestamp captured at last successful sync */
static volatile uint32_t s_unix_base;
/* k_uptime_get_32() value at last successful sync (ms) */
static volatile uint32_t s_uptime_base_ms;
/* True once at least one successful sync has happened */
static volatile bool s_synced;

static void l4_handler(struct net_mgmt_event_callback *cb,
		       uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (event == NET_EVENT_L4_CONNECTED) {
		k_sem_give(&s_net_sem);
	}
}

uint32_t sntp_get_unix_time(void)
{
	if (s_synced) {
		uint32_t elapsed_ms = k_uptime_get_32() - s_uptime_base_ms;

		return s_unix_base + (elapsed_ms / 1000U);
	}
	/* Fallback: seconds since boot */
	return (uint32_t)(k_uptime_get() / 1000);
}

static void sntp_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Wait for L4 connectivity before attempting DNS + UDP to NTP pool */
	k_sem_take(&s_net_sem, K_FOREVER);

	int retries = 0;

	while (retries < SNTP_MAX_RETRIES) {
		struct sntp_time ts;
		int err = sntp_simple(SNTP_SERVER, SNTP_TIMEOUT_MS, &ts);

		if (err == 0) {
			s_uptime_base_ms = k_uptime_get_32();
			s_unix_base      = (uint32_t)ts.seconds;
			s_synced         = true;
			LOG_INF("SNTP sync OK: %u", s_unix_base);
			return;
		}

		LOG_WRN("SNTP sync failed (err %d), retry %d/%d",
			err, retries + 1, SNTP_MAX_RETRIES);
		retries++;
		k_sleep(K_SECONDS(SNTP_RETRY_SEC));
	}

	LOG_ERR("SNTP: max retries reached — using uptime offset");
	/* RING_ONE_TODO: use RTC peripheral to persist time across reboots
	 * nRF54LM20 series has Global RTC in System-OFF; store unix offset in
	 * NVS and restore at boot to avoid the "epoch + uptime" fallback. */
}

void sntp_sync(void)
{
	net_mgmt_init_event_callback(&s_l4_cb, l4_handler,
				     NET_EVENT_L4_CONNECTED);
	net_mgmt_add_event_callback(&s_l4_cb);

	k_thread_create(&s_sntp_thread, s_sntp_stack,
			K_THREAD_STACK_SIZEOF(s_sntp_stack),
			sntp_thread_fn, NULL, NULL, NULL,
			10, 0, K_NO_WAIT);
	k_thread_name_set(&s_sntp_thread, "sntp_sync");
}
