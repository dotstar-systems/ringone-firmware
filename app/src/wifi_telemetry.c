/* Ring•One Firmware · Dotstar Consulting · Apache 2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <net/wifi_ready.h>
#include <net/softap_wifi_provision.h>
#include <psa/protected_storage.h>
#include <stdio.h>
#include <string.h>

#include "wifi_telemetry.h"

LOG_MODULE_REGISTER(wifi_telemetry, LOG_LEVEL_INF);

/* ── Constants ────────────────────────────────────────────────────── */
#define MQTT_BROKER_HOST   CONFIG_RINGONE_MQTT_BROKER_HOST
#define MQTT_BROKER_PORT   CONFIG_RINGONE_MQTT_BROKER_PORT
#define MQTT_KEEPALIVE_SEC 60
#define MQTT_TLS_TAG       1

#define MQTT_THREAD_STACK  6144
#define MQTT_THREAD_PRIO   7

/* Retry waiting for net_if_get_first_wifi(): 50 × 100 ms = 5 s max */
#define WIFI_IFACE_RETRY_COUNT  50
#define WIFI_IFACE_RETRY_MS     100

/* Exponential backoff steps for MQTT reconnect (seconds) */
static const uint32_t s_backoff_sec[] = {5, 10, 30, 60};

/* PSA Protected Storage UIDs for MQTT credentials.
 * Encrypted at rest with a key derived from CRACEN hardware unique key. */
#define PS_UID_MQTT_USERNAME  ((psa_storage_uid_t)0x524E4D51U)  /* "RNMQ" */
#define PS_UID_MQTT_PASSWORD  ((psa_storage_uid_t)0x524E5057U)  /* "RNPW" */
#define MQTT_CRED_MAX_LEN     64

/* ── State ────────────────────────────────────────────────────────── */
static char s_device_id[16];

/* MQTT credential buffers — loaded from PSA PS at init */
static char s_mqtt_user_buf[MQTT_CRED_MAX_LEN];
static char s_mqtt_pass_buf[MQTT_CRED_MAX_LEN];

/* MQTT auth strings — size-delimited, not NUL-terminated */
static struct mqtt_utf8 s_mqtt_user;
static struct mqtt_utf8 s_mqtt_pass;

/* MQTT buffers */
static uint8_t s_rx_buf[512];
static uint8_t s_tx_buf[512];

static struct mqtt_client      s_client;
static struct sockaddr_storage s_broker_addr;

/* Semaphore fires for BOTH wifi_ready=true AND wifi_ready=false events.
 * Check s_wifi_ready_status after taking to know which state fired.
 * Matches the NCS wifi/sta sample pattern for correct race-free handling. */
static K_SEM_DEFINE(s_wifi_hw_sem, 0, 1);
static volatile bool s_wifi_ready_status;

/* s_net_sem: given each time L4 connectivity (DHCP IP) comes up */
static K_SEM_DEFINE(s_net_sem, 0, 1);

/* Volatile — written from net_mgmt thread, read from MQTT thread */
static volatile bool s_net_connected;
static volatile bool s_mqtt_connected;

/* conn_mgr L4 event callback registration */
static struct net_mgmt_event_callback s_l4_cb;

/* Mutex protecting mqtt_publish path (called from main notify thread) */
static K_MUTEX_DEFINE(s_mqtt_mutex);

/* ── Device ID ────────────────────────────────────────────────────── */

static void derive_device_id(char *buf, size_t len)
{
	bt_addr_le_t addr;
	size_t count = 1;

	bt_id_get(&addr, &count);
	snprintf(buf, len, "rng-%02X%02X",
		 addr.a.val[1], addr.a.val[0]);
}

/* ── wifi_ready callback ─────────────────────────────────────────── */
/* Fires for both ready=true (WPA supplicant up) and ready=false (down).
 * Set the bool BEFORE giving the semaphore — avoids a MQTT-thread read
 * of a stale value if the scheduler switches immediately after sem_give. */

static void on_wifi_ready(bool wifi_ready)
{
	s_wifi_ready_status = wifi_ready;
	k_sem_give(&s_wifi_hw_sem);

	if (wifi_ready) {
		LOG_INF("Wi-Fi hardware ready (supplicant up)");
	} else {
		LOG_WRN("Wi-Fi hardware not ready (supplicant down)");
	}
}

/* ── SoftAP provisioning event handler ──────────────────────────── */

static void softap_provision_handler(const struct softap_wifi_provision_evt *evt)
{
	switch (evt->type) {
	case SOFTAP_WIFI_PROVISION_EVT_STARTED:
		LOG_INF("SoftAP provisioning started — connect to 'ringone' AP");
		break;
	case SOFTAP_WIFI_PROVISION_EVT_CLIENT_CONNECTED:
		LOG_INF("SoftAP: provisioning client connected");
		break;
	case SOFTAP_WIFI_PROVISION_EVT_CLIENT_DISCONNECTED:
		LOG_INF("SoftAP: provisioning client disconnected");
		break;
	case SOFTAP_WIFI_PROVISION_EVT_CREDENTIALS_RECEIVED:
		LOG_INF("SoftAP: Wi-Fi credentials received");
		break;
	case SOFTAP_WIFI_PROVISION_EVT_COMPLETED:
		LOG_INF("SoftAP: provisioning completed");
		break;
	case SOFTAP_WIFI_PROVISION_EVT_UNPROVISIONED_REBOOT_NEEDED:
		LOG_WRN("SoftAP: reboot required to complete reset");
		break;
	case SOFTAP_WIFI_PROVISION_EVT_FATAL_ERROR:
		LOG_ERR("SoftAP: fatal provisioning error");
		break;
	default:
		break;
	}
}

/* ── conn_mgr L4 event handler ──────────────────────────────────── */
/* Registered AFTER SoftAP provisioning to avoid spurious L4_CONNECTED
 * events that fire while the device itself acts as AP (gets an IP). */

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (event == NET_EVENT_L4_CONNECTED) {
		struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

		if (ipv4) {
			for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
				struct net_if_addr *a = &ipv4->unicast[i].ipv4;

				if (a->is_used && a->addr_type == NET_ADDR_DHCP) {
					char ipstr[NET_IPV4_ADDR_LEN];

					net_addr_ntop(AF_INET,
						      &a->address.in_addr,
						      ipstr, sizeof(ipstr));
					LOG_INF("Wi-Fi connected, IP: %s", ipstr);
					break;
				}
			}
		}

		s_net_connected = true;
		k_sem_give(&s_net_sem);

	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		s_net_connected = false;
		LOG_WRN("Wi-Fi disconnected — MQTT will reconnect when network returns");
	}
}

/* ── MQTT broker DNS resolve ──────────────────────────────────────── */

static int resolve_broker_addr(void)
{
	struct zsock_addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res;
	char port_str[8];

	snprintf(port_str, sizeof(port_str), "%d", MQTT_BROKER_PORT);

	int err = zsock_getaddrinfo(MQTT_BROKER_HOST, port_str, &hints, &res);

	if (err) {
		LOG_ERR("DNS lookup for %s failed (err %d)", MQTT_BROKER_HOST, err);
		return -EHOSTUNREACH;
	}

	memcpy(&s_broker_addr, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	return 0;
}

/* ── MQTT client setup ────────────────────────────────────────────── */

static void mqtt_event_handler(struct mqtt_client *client,
			       const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			k_mutex_lock(&s_mqtt_mutex, K_FOREVER);
			s_mqtt_connected = true;
			k_mutex_unlock(&s_mqtt_mutex);

			LOG_INF("MQTT connected to %s", MQTT_BROKER_HOST);

			char sub_topic[64];

			snprintf(sub_topic, sizeof(sub_topic),
				 "ring-one/%s/cmd", s_device_id);

			struct mqtt_topic topics[] = {{
				.topic = {
					.utf8 = sub_topic,
					.size = strlen(sub_topic),
				},
				.qos = MQTT_QOS_0_AT_MOST_ONCE,
			}};
			struct mqtt_subscription_list sub_list = {
				.list       = topics,
				.list_count = ARRAY_SIZE(topics),
				.message_id = 1,
			};
			mqtt_subscribe(client, &sub_list);
		} else {
			LOG_ERR("MQTT CONNACK error %d", evt->result);
		}
		break;

	case MQTT_EVT_DISCONNECT:
		k_mutex_lock(&s_mqtt_mutex, K_FOREVER);
		s_mqtt_connected = false;
		k_mutex_unlock(&s_mqtt_mutex);
		LOG_WRN("MQTT disconnected");
		break;

	case MQTT_EVT_PUBLISH: {
		/* Drain incoming PUBLISH — required by the MQTT library even for
		 * QoS 0; bytes must be consumed before the next mqtt_input(). */
		uint32_t len = evt->param.publish.message.payload.len;
		uint8_t  discard[64];

		while (len > 0) {
			uint32_t chunk = MIN(len, sizeof(discard));
			int r = mqtt_read_publish_payload(client, discard, chunk);

			if (r <= 0) {
				break;
			}
			len -= (uint32_t)r;
		}
		break;
	}

	case MQTT_EVT_SUBACK:
		LOG_DBG("MQTT SUBACK received");
		break;

	case MQTT_EVT_PUBACK:
	case MQTT_EVT_PINGRESP:
	default:
		break;
	}
}

static int mqtt_client_connect(void)
{
	static sec_tag_t tls_tags[] = {MQTT_TLS_TAG};

	struct sockaddr_in *broker = (struct sockaddr_in *)&s_broker_addr;

	broker->sin_port = htons(MQTT_BROKER_PORT);

	mqtt_client_init(&s_client);

	s_client.broker           = &s_broker_addr;
	s_client.evt_cb           = mqtt_event_handler;
	s_client.client_id.utf8   = s_device_id;
	s_client.client_id.size   = strlen(s_device_id);
	s_client.protocol_version = MQTT_VERSION_3_1_1;
	s_client.keepalive        = MQTT_KEEPALIVE_SEC;
	s_client.rx_buf           = s_rx_buf;
	s_client.rx_buf_size      = sizeof(s_rx_buf);
	s_client.tx_buf           = s_tx_buf;
	s_client.tx_buf_size      = sizeof(s_tx_buf);

	if (s_mqtt_user.size > 0) {
		s_client.user_name = &s_mqtt_user;
		s_client.password  = &s_mqtt_pass;
	}

	s_client.transport.type                     = MQTT_TRANSPORT_SECURE;
	s_client.transport.tls.config.peer_verify   = TLS_PEER_VERIFY_REQUIRED;
	s_client.transport.tls.config.cipher_list   = NULL;
	s_client.transport.tls.config.sec_tag_list  = tls_tags;
	s_client.transport.tls.config.sec_tag_count = ARRAY_SIZE(tls_tags);
	s_client.transport.tls.config.hostname      = MQTT_BROKER_HOST;

	int err = mqtt_connect(&s_client);

	if (err) {
		LOG_ERR("mqtt_connect failed (err %d)", err);
	}

	return err;
}

/* ── MQTT thread ──────────────────────────────────────────────────── */

static K_THREAD_STACK_DEFINE(s_mqtt_stack, MQTT_THREAD_STACK);
static struct k_thread s_mqtt_thread;

static void mqtt_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Phase 0: wait for net_if_get_first_wifi() to succeed.
	 * The nRF7002 companion firmware is loaded asynchronously after boot;
	 * the Wi-Fi interface isn't registered until that completes. */
	struct net_if *iface = NULL;

	for (int i = 0; i < WIFI_IFACE_RETRY_COUNT; i++) {
		iface = net_if_get_first_wifi();
		if (iface) {
			break;
		}
		k_sleep(K_MSEC(WIFI_IFACE_RETRY_MS));
	}

	if (!iface) {
		LOG_ERR("No Wi-Fi interface after %d ms — aborting",
			WIFI_IFACE_RETRY_COUNT * WIFI_IFACE_RETRY_MS);
		return;
	}

	/* Register wifi_ready callback with the explicit iface handle.
	 * Passing NULL causes register_wifi_ready_callback to call
	 * net_if_get_first_wifi() internally at registration time, which
	 * fails (-ENODEV) if called too early in boot. */
	wifi_ready_callback_t cb = {.wifi_ready_cb = on_wifi_ready};
	int err = register_wifi_ready_callback(cb, iface);

	if (err) {
		LOG_ERR("register_wifi_ready_callback failed (err %d)", err);
		return;
	}

	/* Phase 1: block until WPA supplicant signals hardware ready.
	 * The semaphore fires for BOTH ready=true and ready=false — check
	 * the bool. If false, keep waiting (supplicant hasn't finished init). */
	LOG_INF("Waiting for Wi-Fi hardware ready...");
	while (true) {
		k_sem_take(&s_wifi_hw_sem, K_FOREVER);
		if (s_wifi_ready_status) {
			break;
		}
		LOG_WRN("Wi-Fi HW not ready yet — waiting...");
	}

	/* Phase 2: SoftAP provisioning.
	 * If Wi-Fi credentials are already stored (typical after first boot),
	 * softap_wifi_provision_start() returns -EALREADY immediately.
	 * On first boot or after a credential reset, it starts the HTTPS AP
	 * and blocks until the companion app delivers credentials. */
	err = softap_wifi_provision_init(softap_provision_handler);
	if (err) {
		LOG_ERR("softap_wifi_provision_init failed (err %d)", err);
		return;
	}

	err = conn_mgr_all_if_up(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_up failed (err %d)", err);
		return;
	}

	err = softap_wifi_provision_start();
	if (err == -EALREADY) {
		LOG_INF("Wi-Fi credentials found — skipping SoftAP provisioning");
	} else if (err != 0) {
		LOG_ERR("softap_wifi_provision_start failed (err %d)", err);
		return;
	} else {
		LOG_INF("SoftAP provisioning complete");
	}

	/* Phase 3: register L4 callbacks AFTER provisioning.
	 * Registering before softap_wifi_provision_start() would fire a
	 * spurious NET_EVENT_L4_CONNECTED while the device acts as AP server
	 * and receives its own DHCP-assigned IP, confusing the MQTT loop. */
	net_mgmt_init_event_callback(&s_l4_cb, l4_event_handler,
				     NET_EVENT_L4_CONNECTED |
				     NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&s_l4_cb);

	err = conn_mgr_all_if_connect(true);
	if (err) {
		LOG_ERR("conn_mgr_all_if_connect failed (err %d)", err);
	}

	uint32_t backoff_idx = 0;

	while (true) {
		/* Phase 4: wait for L4 connectivity (DHCP IP assigned) */
		if (!s_net_connected) {
			LOG_INF("MQTT thread: waiting for network...");
			k_sem_take(&s_net_sem, K_FOREVER);
		}

		/* Phase 5: MQTT connect + I/O while network is up */
		while (s_net_connected) {
			if (!s_mqtt_connected) {
				if (resolve_broker_addr() != 0 ||
				    mqtt_client_connect() != 0) {
					uint32_t delay = s_backoff_sec[backoff_idx];

					LOG_WRN("MQTT connect failed — retry in %us",
						delay);
					k_sleep(K_SECONDS(delay));
					if (backoff_idx <
					    ARRAY_SIZE(s_backoff_sec) - 1) {
						backoff_idx++;
					}
					continue;
				}
				backoff_idx = 0;
			}

			int rc = mqtt_input(&s_client);

			if (rc && rc != -EAGAIN) {
				LOG_WRN("mqtt_input error %d — reconnecting", rc);
				k_mutex_lock(&s_mqtt_mutex, K_FOREVER);
				s_mqtt_connected = false;
				k_mutex_unlock(&s_mqtt_mutex);
				mqtt_disconnect(&s_client, NULL);
				continue;
			}

			mqtt_live(&s_client);
		}

		/* Network went down — disconnect MQTT cleanly */
		if (s_mqtt_connected) {
			mqtt_disconnect(&s_client, NULL);
			k_mutex_lock(&s_mqtt_mutex, K_FOREVER);
			s_mqtt_connected = false;
			k_mutex_unlock(&s_mqtt_mutex);
		}
		backoff_idx = 0;
	}
}

/* ── Public API ───────────────────────────────────────────────────── */

int wifi_telemetry_init(void)
{
	derive_device_id(s_device_id, sizeof(s_device_id));
	LOG_INF("Wi-Fi telemetry device ID: %s", s_device_id);

	/* Load MQTT credentials from PSA Protected Storage (CRACEN HUK-encrypted).
	 * Falls back to Kconfig defaults on first boot or when PS is empty — this
	 * makes dev boards work out of the box while production devices get real
	 * credentials written by the factory provisioning tool via
	 * wifi_telemetry_provision_mqtt_creds(). */
	size_t user_len = 0, pass_len = 0;

	psa_status_t ps_err = psa_ps_get(PS_UID_MQTT_USERNAME, 0,
					 sizeof(s_mqtt_user_buf) - 1,
					 s_mqtt_user_buf, &user_len);
	if (ps_err == PSA_SUCCESS && user_len > 0) {
		s_mqtt_user_buf[user_len] = '\0';
		psa_ps_get(PS_UID_MQTT_PASSWORD, 0,
			   sizeof(s_mqtt_pass_buf) - 1,
			   s_mqtt_pass_buf, &pass_len);
		s_mqtt_pass_buf[pass_len] = '\0';
		LOG_INF("MQTT credentials loaded from Protected Storage");
	} else {
		strncpy(s_mqtt_user_buf, CONFIG_RINGONE_MQTT_USERNAME,
			sizeof(s_mqtt_user_buf) - 1);
		strncpy(s_mqtt_pass_buf, CONFIG_RINGONE_MQTT_PASSWORD,
			sizeof(s_mqtt_pass_buf) - 1);
		user_len = strlen(s_mqtt_user_buf);
		pass_len = strlen(s_mqtt_pass_buf);
		LOG_WRN("MQTT credentials from Kconfig defaults (dev mode)");
	}

	s_mqtt_user = (struct mqtt_utf8){
		.utf8 = s_mqtt_user_buf,
		.size = user_len,
	};
	s_mqtt_pass = (struct mqtt_utf8){
		.utf8 = s_mqtt_pass_buf,
		.size = pass_len,
	};

	/* Spawn MQTT thread — it handles the full Wi-Fi + provisioning lifecycle.
	 * Returns immediately; BLE advertising and sensor loop run concurrently. */
	k_thread_create(&s_mqtt_thread, s_mqtt_stack,
			K_THREAD_STACK_SIZEOF(s_mqtt_stack),
			mqtt_thread_fn, NULL, NULL, NULL,
			MQTT_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&s_mqtt_thread, "mqtt_telemetry");

	return 0;
}

int wifi_telemetry_provision_mqtt_creds(const char *username, const char *password)
{
	if (!username || !password) {
		return -EINVAL;
	}

	size_t ulen = strlen(username);
	size_t plen = strlen(password);

	if (ulen == 0 || ulen >= MQTT_CRED_MAX_LEN ||
	    plen >= MQTT_CRED_MAX_LEN) {
		return -EINVAL;
	}

	psa_status_t err = psa_ps_set(PS_UID_MQTT_USERNAME,
				      ulen, username,
				      PSA_STORAGE_FLAG_NONE);
	if (err != PSA_SUCCESS) {
		LOG_ERR("psa_ps_set username failed (%d)", (int)err);
		return -EIO;
	}

	err = psa_ps_set(PS_UID_MQTT_PASSWORD,
			 plen, password,
			 PSA_STORAGE_FLAG_NONE);
	if (err != PSA_SUCCESS) {
		LOG_ERR("psa_ps_set password failed (%d)", (int)err);
		return -EIO;
	}

	LOG_INF("MQTT credentials stored in Protected Storage");
	return 0;
}

int wifi_telemetry_publish(const ringone_telemetry_t *payload)
{
	if (!payload) {
		return -EINVAL;
	}

	k_mutex_lock(&s_mqtt_mutex, K_FOREVER);
	bool connected = s_mqtt_connected;
	k_mutex_unlock(&s_mqtt_mutex);

	if (!connected) {
		return -ENOTCONN;
	}

	uint32_t ts = payload->timestamp != 0
		? payload->timestamp
		: (uint32_t)(k_uptime_get() / 1000);

	char json_buf[256];
	int  json_len;

	json_len = snprintf(json_buf, sizeof(json_buf),
		"{\"device_id\":\"%s\","
		"\"ts\":%u,"
		"\"hr\":%u,"
		"\"spo2\":%u,"
		"\"temp\":%.2f,"
		"\"steps\":%u,"
		"\"battery\":%u,"
		"\"rssi_ble\":%d}",
		s_device_id,
		ts,
		(unsigned)payload->heart_rate,
		(unsigned)payload->spo2,
		(double)payload->temperature / 100.0,
		(unsigned)payload->steps,
		(unsigned)payload->battery,
		(int)payload->rssi_ble);

	if (json_len < 0 || json_len >= (int)sizeof(json_buf)) {
		LOG_ERR("JSON payload too large");
		return -EMSGSIZE;
	}

	char topic_buf[64];

	snprintf(topic_buf, sizeof(topic_buf),
		 "ring-one/%s/telemetry", s_device_id);

	struct mqtt_publish_param pub = {
		.message = {
			.topic = {
				.topic = {
					.utf8 = topic_buf,
					.size = strlen(topic_buf),
				},
				.qos = MQTT_QOS_0_AT_MOST_ONCE,
			},
			.payload = {
				.data = json_buf,
				.len  = (uint32_t)json_len,
			},
		},
		.message_id  = 0,
		.dup_flag    = 0,
		.retain_flag = 0,
	};

	int err = mqtt_publish(&s_client, &pub);

	if (err) {
		LOG_ERR("mqtt_publish failed (err %d)", err);
		k_mutex_lock(&s_mqtt_mutex, K_FOREVER);
		s_mqtt_connected = false;
		k_mutex_unlock(&s_mqtt_mutex);
	}

	return err;
}

bool wifi_telemetry_connected(void)
{
	k_mutex_lock(&s_mqtt_mutex, K_FOREVER);
	bool c = s_mqtt_connected;
	k_mutex_unlock(&s_mqtt_mutex);
	return c;
}
