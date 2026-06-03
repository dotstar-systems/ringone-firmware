/* Ring•One Firmware · Dotstar Consulting · Apache 2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi_credentials.h>
#include <dk_buttons_and_leds.h>
#if IS_ENABLED(CONFIG_SOFTAP_WIFI_PROVISION)
#include <net/softap_wifi_provision.h>
#endif
#include <net/wifi_ready.h>
#include <string.h>

#include "wifi_prov.h"

LOG_MODULE_REGISTER(wifi_prov, LOG_LEVEL_INF);

/* RING_ONE_TODO: add mDNS/DNS-SD so captive portal redirects
 * automatically on iOS/Android.  Register _http._tcp service with
 * CONFIG_MDNS_RESPONDER=y and CONFIG_DNS_SD=y (already enabled in prj.conf).
 * Hook into softap_wifi_provision_start() lifecycle events. */

/* RING_ONE_TODO: add WPA3-SAE support for enterprise deployments.
 * CONFIG_WPA_SUPP_WPA3=y (i.e. CONFIG_WIFI_NM_WPA_SUPPLICANT_WPA3_*) is
 * already set; wire it up in wifi_prov_connect() when security type is SAE. */

#define PROV_THREAD_STACK  16384
#define PROV_THREAD_PRIO   7

/* Backoff steps for reconnect: 5 s → 15 s → 30 s → 60 s → 60 s */
static const uint32_t s_backoff_sec[] = {5, 15, 30, 60, 60};

/* ── State ────────────────────────────────────────────────────────── */
static volatile ringone_wifi_status_t s_status = WIFI_STATUS_NOT_PROVISIONED;
static const struct bt_gatt_attr     *s_status_attr;

/* BLE-assisted provisioning: SSID/password received from GATT writes */
static char     s_ble_ssid[33];     /* SSID max 32 + NUL */
static char     s_ble_pass[65];     /* WPA2 max 64 + NUL */
static bool     s_ble_ssid_ready;
static bool     s_ble_pass_ready;
static K_MUTEX_DEFINE(s_ble_mutex);
static K_SEM_DEFINE(s_ble_cred_sem, 0, 1);

/* conn_mgr L4 event callback */
static struct net_mgmt_event_callback s_l4_cb;
static K_SEM_DEFINE(s_net_sem, 0, 1);
static volatile bool s_net_connected;

/* wifi_ready semaphore */
static K_SEM_DEFINE(s_wifi_hw_sem, 0, 1);
static volatile bool s_wifi_ready_status;

/* Long-press button flag */
static volatile bool s_softap_requested;

/* PSM management: disable for 120 s after fresh provisioning so mDNS SD
 * remains stable while the nRF Wi-Fi Provisioner app confirms success.
 * Matches SOFTAP_WIFI_PROVISION_SAMPLE_PSM_DISABLED_SECONDS = 120. */
#define PSM_DISABLED_POST_PROV_SEC 120
static volatile bool s_psm_disable_on_connect;
static void psm_reenable_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(psm_reenable_work, psm_reenable_handler);

static void psm_reenable_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	struct net_if *iface = net_if_get_first_wifi();

	if (!iface) {
		return;
	}
	struct wifi_ps_params ps = {.enabled = WIFI_PS_ENABLED};
	int err = net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps, sizeof(ps));

	if (err) {
		LOG_WRN("Failed to re-enable Wi-Fi PSM (err %d)", err);
	} else {
		LOG_INF("Wi-Fi PSM re-enabled after %d s post-provisioning window",
			PSM_DISABLED_POST_PROV_SEC);
	}
}

/* ── Status notify helper ─────────────────────────────────────────── */

static void set_status(ringone_wifi_status_t new_status)
{
	s_status = new_status;
	if (s_status_attr) {
		uint8_t val = (uint8_t)new_status;

		bt_gatt_notify(NULL, s_status_attr, &val, sizeof(val));
	}
}

/* ── GATT write callbacks (called from main.c) ────────────────────── */

void wifi_prov_on_ssid_write(const uint8_t *data, uint16_t len)
{
	if (!data || len == 0 || len > 32) {
		return;
	}
	k_mutex_lock(&s_ble_mutex, K_FOREVER);
	memcpy(s_ble_ssid, data, len);
	s_ble_ssid[len]  = '\0';
	s_ble_ssid_ready = true;
	LOG_INF("Wi-Fi SSID received via BLE (%u bytes)", len);
	k_mutex_unlock(&s_ble_mutex);
}

void wifi_prov_on_password_write(const uint8_t *data, uint16_t len)
{
	if (!data || len > 64) {
		return;
	}
	k_mutex_lock(&s_ble_mutex, K_FOREVER);
	if (len > 0) {
		memcpy(s_ble_pass, data, len);
	}
	s_ble_pass[len]  = '\0';
	s_ble_pass_ready = true;
	LOG_INF("Wi-Fi password received via BLE");

	/* Password write triggers connection attempt */
	if (s_ble_ssid_ready) {
		k_sem_give(&s_ble_cred_sem);
	}
	k_mutex_unlock(&s_ble_mutex);
}

void wifi_prov_set_status_attr(const struct bt_gatt_attr *attr)
{
	s_status_attr = attr;
}

ringone_wifi_status_t wifi_prov_get_status(void)
{
	return s_status;
}

/* ── wifi_ready callback ──────────────────────────────────────────── */

static void on_wifi_ready(bool ready)
{
	s_wifi_ready_status = ready;
	k_sem_give(&s_wifi_hw_sem);
	if (ready) {
		LOG_INF("Wi-Fi hardware ready");
	} else {
		LOG_WRN("Wi-Fi hardware not ready");
	}
}

/* ── SoftAP provisioning event handler ───────────────────────────── */

#if IS_ENABLED(CONFIG_SOFTAP_WIFI_PROVISION)
static void softap_event_handler(const struct softap_wifi_provision_evt *evt)
{
	switch (evt->type) {
	case SOFTAP_WIFI_PROVISION_EVT_STARTED:
		LOG_INF("SoftAP '%s' started — connect to provision Ring-One",
			CONFIG_SOFTAP_WIFI_PROVISION_SSID);
		break;
	case SOFTAP_WIFI_PROVISION_EVT_CLIENT_CONNECTED:
		LOG_INF("SoftAP: provisioning client connected");
		break;
	case SOFTAP_WIFI_PROVISION_EVT_CREDENTIALS_RECEIVED:
		LOG_INF("SoftAP: Wi-Fi credentials received");
		break;
	case SOFTAP_WIFI_PROVISION_EVT_COMPLETED:
		LOG_INF("SoftAP: provisioning complete");
		break;
	case SOFTAP_WIFI_PROVISION_EVT_FATAL_ERROR:
		LOG_ERR("SoftAP: fatal provisioning error");
		break;
	default:
		break;
	}
}
#endif /* IS_ENABLED(CONFIG_SOFTAP_WIFI_PROVISION) */

/* ── L4 event handler ─────────────────────────────────────────────── */

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (event == NET_EVENT_L4_CONNECTED) {
		struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

		if (ipv4) {
			for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
				struct net_if_addr *a =
					&ipv4->unicast[i].ipv4;

				if (a->is_used &&
				    a->addr_type == NET_ADDR_DHCP) {
					char ipstr[NET_IPV4_ADDR_LEN];

					net_addr_ntop(AF_INET,
						      &a->address.in_addr,
						      ipstr, sizeof(ipstr));
					LOG_INF("Ring-One Wi-Fi: [%s] IP:%s",
						s_ble_ssid_ready
							? s_ble_ssid : "?",
						ipstr);
					break;
				}
			}
		}
		/* After fresh provisioning, disable PSM for PSM_DISABLED_POST_PROV_SEC
		 * so mDNS SD stays stable while the provisioner app confirms
		 * success.  On normal reboots with stored credentials, PSM is
		 * left on for battery efficiency. */
		if (s_psm_disable_on_connect) {
			s_psm_disable_on_connect = false;
			struct wifi_ps_params ps = {.enabled = WIFI_PS_DISABLED};
			int ps_err = net_mgmt(NET_REQUEST_WIFI_PS, iface,
					      &ps, sizeof(ps));
			if (ps_err) {
				LOG_WRN("Failed to disable Wi-Fi PSM (err %d)",
					ps_err);
			} else {
				LOG_INF("Wi-Fi PSM disabled for %d s "
					"(mDNS SD post-provisioning window)",
					PSM_DISABLED_POST_PROV_SEC);
				k_work_schedule(&psm_reenable_work,
						K_SECONDS(
						PSM_DISABLED_POST_PROV_SEC));
			}
		}

		s_net_connected = true;
		set_status(WIFI_STATUS_CONNECTED);
		k_sem_give(&s_net_sem);

	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		s_net_connected = false;
		set_status(WIFI_STATUS_CONNECTING);
		LOG_WRN("Wi-Fi disconnected — attempting reconnect");
	}
}

/* ── Button handler (long-press → SoftAP) ────────────────────────── */

static uint32_t s_btn_press_time_ms;

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
#define SOFTAP_LONGPRESS_MS 5000U
	if (has_changed & DK_BTN1_MSK) {
		if (button_state & DK_BTN1_MSK) {
			s_btn_press_time_ms = k_uptime_get_32();
		} else {
			uint32_t held = k_uptime_get_32() - s_btn_press_time_ms;

			if (held >= SOFTAP_LONGPRESS_MS) {
				LOG_INF("Button 1 long-press — requesting SoftAP mode");
				s_softap_requested = true;
			}
		}
	}
}

/* ── Connect using BLE-received credentials ──────────────────────── */

static int ble_connect(void)
{
	char ssid[33], pass[65];

	k_mutex_lock(&s_ble_mutex, K_FOREVER);
	strncpy(ssid, s_ble_ssid, sizeof(ssid) - 1);
	ssid[sizeof(ssid) - 1] = '\0';
	strncpy(pass, s_ble_pass, sizeof(pass) - 1);
	pass[sizeof(pass) - 1] = '\0';
	k_mutex_unlock(&s_ble_mutex);

	int err = wifi_credentials_set_personal(
		ssid, strlen(ssid),
		WIFI_SECURITY_TYPE_PSK,
		NULL, 0,             /* bssid not required */
		pass, strlen(pass),
		0, 0, 0);            /* flags=0, channel=0, timeout=0 */

	if (err && err != -EALREADY) {
		LOG_ERR("wifi_credentials_set_personal failed (err %d)", err);
		return err;
	}

	err = conn_mgr_all_if_connect(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_connect failed (err %d)", err);
	}
	return err;
}

/* ── Provisioning thread ──────────────────────────────────────────── */

static K_THREAD_STACK_DEFINE(s_prov_stack, PROV_THREAD_STACK);
static struct k_thread s_prov_thread;

static void prov_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int err;

	/* Phase 0: wait for nRF7002 firmware load */
	struct net_if *iface = NULL;

	for (int i = 0; i < 50; i++) {
		iface = net_if_get_first_wifi();
		if (iface) {
			break;
		}
		k_sleep(K_MSEC(100));
	}
	if (!iface) {
		LOG_ERR("No Wi-Fi interface after 5 s — provisioning aborted");
		return;
	}

	wifi_ready_callback_t wcb = {.wifi_ready_cb = on_wifi_ready};

	err = register_wifi_ready_callback(wcb, iface);
	if (err) {
		LOG_ERR("register_wifi_ready_callback failed (err %d)", err);
		return;
	}

	LOG_INF("Waiting for Wi-Fi hardware ready...");
	while (true) {
		k_sem_take(&s_wifi_hw_sem, K_FOREVER);
		if (s_wifi_ready_status) {
			break;
		}
		LOG_WRN("Wi-Fi HW not ready yet — waiting...");
	}

	err = conn_mgr_all_if_up(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_up failed (err %d)", err);
		return;
	}

	/* Check if we have stored credentials already */
	bool have_creds = !wifi_credentials_is_empty();

#if IS_ENABLED(CONFIG_SOFTAP_WIFI_PROVISION)
	/* Phase 1: SoftAP provisioning (NCS library handles the HTTP portal).
	 * Returns -EALREADY immediately if credentials already exist. */
	err = softap_wifi_provision_init(softap_event_handler);
	if (err) {
		LOG_ERR("softap_wifi_provision_init failed (err %d)", err);
		return;
	}

	if (!have_creds || s_softap_requested) {
		if (s_softap_requested) {
			LOG_INF("SoftAP mode requested by button long-press");
		}

		err = softap_wifi_provision_start();
		if (err == -EALREADY) {
			LOG_INF("Credentials exist — skipping SoftAP");
			have_creds = true;
		} else if (err != 0) {
			LOG_ERR("softap_wifi_provision_start failed (err %d)",
				err);
		} else {
			LOG_INF("SoftAP provisioning complete");
			have_creds = true;
			/* Disable PSM on next connect for mDNS SD stability */
			s_psm_disable_on_connect = true;
		}
	} else {
		LOG_INF("Wi-Fi credentials found — skipping SoftAP");
	}
#else
	LOG_INF("SoftAP provisioning not compiled — using BLE-assisted only");
#endif /* IS_ENABLED(CONFIG_SOFTAP_WIFI_PROVISION) */

	/* Phase 2: Register L4 callbacks AFTER provisioning to avoid spurious
	 * NET_EVENT_L4_CONNECTED while the device acts as AP. */
	net_mgmt_init_event_callback(&s_l4_cb, l4_event_handler,
				     NET_EVENT_L4_CONNECTED |
				     NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&s_l4_cb);

	/* If BLE credentials arrived while we were waiting, connect with them */
	if (s_ble_ssid_ready && s_ble_pass_ready) {
		set_status(WIFI_STATUS_CONNECTING);
		err = ble_connect();
		if (err) {
			set_status(WIFI_STATUS_FAILED);
		}
	} else {
		err = conn_mgr_all_if_connect(true);
		if (err) {
			LOG_ERR("conn_mgr_all_if_connect failed (err %d)", err);
		}
	}

	/* Phase 3: Wait for BLE-assisted credentials if not yet provisioned */
	if (!have_creds && !s_ble_ssid_ready) {
		LOG_INF("Waiting for BLE provisioning (write WiFi-SSID and "
			"WiFi-Password characteristics)...");
		k_sem_take(&s_ble_cred_sem, K_FOREVER);
		set_status(WIFI_STATUS_CONNECTING);
		err = ble_connect();
		if (err) {
			set_status(WIFI_STATUS_FAILED);
			k_sleep(K_SECONDS(10));
			/* Retry once */
			err = ble_connect();
			if (err) {
				set_status(WIFI_STATUS_FAILED);
			}
		}
	}

	/* Phase 4: connection monitor — reconnect with exponential backoff */
	uint32_t backoff_idx = 0;

	while (true) {
		k_sem_take(&s_net_sem, K_SECONDS(60));

		if (!s_net_connected) {
			uint32_t delay = s_backoff_sec[
				MIN(backoff_idx,
				    ARRAY_SIZE(s_backoff_sec) - 1)];

			LOG_WRN("Wi-Fi not connected — retry in %u s", delay);
			k_sleep(K_SECONDS(delay));

			err = conn_mgr_all_if_connect(true);
			if (err) {
				LOG_ERR("Reconnect failed (err %d)", err);
				if (backoff_idx < ARRAY_SIZE(s_backoff_sec) - 1) {
					backoff_idx++;
				}
			} else {
				backoff_idx = 0;
			}
		} else {
			backoff_idx = 0;
		}
	}
}

/* ── Public init ──────────────────────────────────────────────────── */

int wifi_prov_init(void)
{
	int err;

	/* Register button handler for SoftAP long-press trigger */
	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_WRN("dk_buttons_init failed (err %d) — "
			"SoftAP button trigger disabled", err);
	}

	k_thread_create(&s_prov_thread, s_prov_stack,
			K_THREAD_STACK_SIZEOF(s_prov_stack),
			prov_thread_fn, NULL, NULL, NULL,
			PROV_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&s_prov_thread, "wifi_prov");

	return 0;
}
