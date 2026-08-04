/* Ring•One Firmware · Dotstar Systems · Apache 2.0 */

/*
 * PATH C — MQTT/TLS bidirectional to HiveMQ Cloud
 *
 * PUBLISH:   ring-one/<device_id>/telemetry  (QoS 0, lossy OK)
 * SUBSCRIBE: ring-one/<device_id>/cmd        (QoS 1, reliable)
 *
 * Commands handled on /cmd:
 *   {"cmd":"ping"}                → pong to /status
 *   {"cmd":"reboot"}              → sys_reboot() after 3 s
 *   {"cmd":"set_interval","value":<s>}  → update telemetry interval
 *   {"cmd":"get_status"}          → full status JSON to /status
 */

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
#include <zephyr/sys/reboot.h>
#include <psa/protected_storage.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_client.h"
#include "sntp_sync.h"
#include "watchdog.h"

LOG_MODULE_REGISTER(mqtt_client, LOG_LEVEL_INF);

/* RING_ONE_TODO: implement LWT (Last Will Testament)
 * topic:   ring-one/<device_id>/status
 * payload: {"online": false}
 * This notifies cloud dashboards when the device goes offline unexpectedly.
 * Set via mqtt_client.will_message before calling mqtt_connect(). */

#define MQTT_BROKER_HOST    CONFIG_RINGONE_MQTT_BROKER_HOST
#define MQTT_BROKER_PORT    CONFIG_RINGONE_MQTT_BROKER_PORT
#define MQTT_KEEPALIVE_SEC  60

#define MQTT_THREAD_STACK   8192
#define MQTT_THREAD_PRIO    9

/* Exponential backoff: 5 s → 10 s → 30 s → 60 s */
static const uint32_t s_backoff_sec[] = {5, 10, 30, 60};

/* PSA Protected Storage UIDs for MQTT credentials */
#define PS_UID_MQTT_USERNAME  ((psa_storage_uid_t)0x524E4D51U)  /* "RNMQ" */
#define PS_UID_MQTT_PASSWORD  ((psa_storage_uid_t)0x524E5057U)  /* "RNPW" */
#define MQTT_CRED_MAX_LEN     64

/* Dynamic telemetry interval (changed by set_interval command) */
static volatile uint32_t s_telemetry_interval_sec =
	CONFIG_RINGONE_TELEMETRY_INTERVAL_SEC;

/* ── State ────────────────────────────────────────────────────────── */
/* Hostname stripped of any trailing ":port" from RINGONE_MQTT_BROKER_HOST.
 * The Kconfig default may be set to "host:8883" for convenience; we split
 * it so DNS lookup and TLS SNI both receive a bare hostname. */
static char s_broker_host[128];

static char s_device_id[16];
static char s_mqtt_user_buf[MQTT_CRED_MAX_LEN];
static char s_mqtt_pass_buf[MQTT_CRED_MAX_LEN];
static struct mqtt_utf8 s_mqtt_user;
static struct mqtt_utf8 s_mqtt_pass;

static uint8_t s_rx_buf[512];
static uint8_t s_tx_buf[512];
static struct mqtt_client      s_client;
static struct sockaddr_storage s_broker_addr;

static K_MUTEX_DEFINE(s_mutex);
static volatile bool s_mqtt_connected;

/* Outbound publish queue — keeps mqtt_publish()/TLS-encrypt off sysworkq */
K_MSGQ_DEFINE(s_pub_queue, sizeof(ringone_data_t), 4, 4);

/* L4 connectivity */
static struct net_mgmt_event_callback s_l4_cb;
static K_SEM_DEFINE(s_net_sem, 0, 1);
static volatile bool s_net_connected;

/* ── Device ID ────────────────────────────────────────────────────── */

static void derive_device_id(void)
{
	bt_addr_le_t addr;
	size_t count = 1;

	bt_id_get(&addr, &count);
	snprintf(s_device_id, sizeof(s_device_id), "ring-one-%02X%02X",
		 addr.a.val[1], addr.a.val[0]);
}

/* ── Command handling ─────────────────────────────────────────────── */

static void publish_str(const char *topic, const char *payload)
{
	struct mqtt_publish_param pub = {
		.message = {
			.topic = {
				.topic = {
					.utf8 = topic,
					.size = strlen(topic),
				},
				.qos = MQTT_QOS_0_AT_MOST_ONCE,
			},
			.payload = {
				.data = (uint8_t *)payload,
				.len  = (uint32_t)strlen(payload),
			},
		},
		.message_id  = 0,
		.dup_flag    = 0,
		.retain_flag = 0,
	};
	mqtt_publish(&s_client, &pub);
}

static void handle_command(const char *payload, uint32_t len)
{
	ARG_UNUSED(len);

	char status_topic[64];

	snprintf(status_topic, sizeof(status_topic),
		 "ring-one/%s/status", s_device_id);

	if (strstr(payload, "\"cmd\":\"ping\"")) {
		char pong[64];

		snprintf(pong, sizeof(pong),
			 "{\"cmd\":\"pong\",\"ts\":%u}",
			 sntp_get_unix_time());
		publish_str(status_topic, pong);
		LOG_INF("MQTT: ping → pong");

	} else if (strstr(payload, "\"cmd\":\"reboot\"")) {
		LOG_WRN("OTA reboot command received — rebooting in 3 s");
		k_sleep(K_SECONDS(3));
		sys_reboot(SYS_REBOOT_WARM);

	} else if (strstr(payload, "\"cmd\":\"set_interval\"")) {
		const char *vp = strstr(payload, "\"value\":");

		if (vp) {
			int val;

			if (sscanf(vp + 8, "%d", &val) == 1 &&
			    val >= 10 && val <= 300) {
				s_telemetry_interval_sec = (uint32_t)val;
				LOG_INF("Telemetry interval set to %d s", val);
			} else {
				LOG_WRN("set_interval: invalid value");
			}
		}

	} else if (strstr(payload, "\"cmd\":\"get_status\"")) {
		char status[256];

		snprintf(status, sizeof(status),
			 "{\"device_id\":\"%s\",\"firmware\":\"0.1.0\","
			 "\"uptime_s\":%u,\"wifi_rssi\":-60,"
			 "\"heap_free\":%u,\"battery\":85,"
			 "\"crash_count\":%u}",
			 s_device_id,
			 (uint32_t)(k_uptime_get() / 1000),
			 0U,  /* RING_ONE_TODO: query sys heap stats */
			 watchdog_get_crash_count());
		publish_str(status_topic, status);
	}
}

/* ── MQTT event handler ───────────────────────────────────────────── */

static void mqtt_evt_handler(struct mqtt_client *client,
			     const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			k_mutex_lock(&s_mutex, K_FOREVER);
			s_mqtt_connected = true;
			k_mutex_unlock(&s_mutex);
			LOG_INF("MQTT connected to %s", MQTT_BROKER_HOST);

			/* Subscribe to /cmd with QoS 1 (reliable) */
			char sub_topic[64];

			snprintf(sub_topic, sizeof(sub_topic),
				 "ring-one/%s/cmd", s_device_id);

			struct mqtt_topic topics[] = {{
				.topic = {
					.utf8 = sub_topic,
					.size = strlen(sub_topic),
				},
				.qos = MQTT_QOS_1_AT_LEAST_ONCE,
			}};
			struct mqtt_subscription_list sub = {
				.list       = topics,
				.list_count = ARRAY_SIZE(topics),
				.message_id = 1,
			};
			mqtt_subscribe(client, &sub);
		} else {
			LOG_ERR("MQTT CONNACK error %d", evt->result);
		}
		break;

	case MQTT_EVT_DISCONNECT:
		k_mutex_lock(&s_mutex, K_FOREVER);
		s_mqtt_connected = false;
		k_mutex_unlock(&s_mutex);
		LOG_WRN("MQTT disconnected");
		break;

	case MQTT_EVT_PUBLISH: {
		uint32_t len = evt->param.publish.message.payload.len;
		uint8_t  buf[128];

		if (len >= sizeof(buf)) {
			/* Too large: drain and discard */
			uint8_t discard[64];

			while (len > 0) {
				uint32_t chunk = MIN(len, sizeof(discard));
				int r = mqtt_read_publish_payload(
					client, discard, chunk);

				if (r <= 0) {
					break;
				}
				len -= (uint32_t)r;
			}
			break;
		}

		int r = mqtt_read_publish_payload(client, buf, len);

		if (r == (int)len) {
			buf[len] = '\0';
			handle_command((const char *)buf, len);
		}

		/* ACK QoS 1 */
		if (evt->param.publish.message.topic.qos ==
		    MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = {
				.message_id =
					evt->param.publish.message_id,
			};
			mqtt_publish_qos1_ack(client, &ack);
		}
		break;
	}

	case MQTT_EVT_SUBACK:
		LOG_DBG("MQTT SUBACK");
		break;

	case MQTT_EVT_PUBACK:
	case MQTT_EVT_PINGRESP:
	default:
		break;
	}
}

/* ── DNS + connect ────────────────────────────────────────────────── */

static int resolve_broker(void)
{
	struct zsock_addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res;
	char port_str[8];

	snprintf(port_str, sizeof(port_str), "%d", MQTT_BROKER_PORT);

	int err = zsock_getaddrinfo(s_broker_host, port_str, &hints, &res);

	if (err) {
		LOG_ERR("DNS lookup %s failed (err %d)", s_broker_host, err);
		return -EHOSTUNREACH;
	}
	memcpy(&s_broker_addr, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);
	return 0;
}

static int broker_connect(void)
{
	struct sockaddr_in *broker = (struct sockaddr_in *)&s_broker_addr;

	broker->sin_port = htons(MQTT_BROKER_PORT);

	mqtt_client_init(&s_client);
	s_client.broker           = &s_broker_addr;
	s_client.evt_cb           = mqtt_evt_handler;
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

	/* RING_ONE_TODO: register HiveMQ CA cert via tls_credential_add() and
	 * change peer_verify back to TLS_PEER_VERIFY_REQUIRED once provisioned.
	 * sec_tag_list must stay NULL until a credential is registered — Zephyr
	 * validates the tag list on setsockopt even when peer_verify is NONE. */
	s_client.transport.type = MQTT_TRANSPORT_SECURE;
	s_client.transport.tls.config.peer_verify   = TLS_PEER_VERIFY_NONE;
	s_client.transport.tls.config.cipher_list   = NULL;
	s_client.transport.tls.config.sec_tag_list  = NULL;
	s_client.transport.tls.config.sec_tag_count = 0;
	s_client.transport.tls.config.hostname      = s_broker_host;

	int err = mqtt_connect(&s_client);

	if (err) {
		LOG_ERR("mqtt_connect failed (err %d)", err);
	}
	return err;
}

/* ── L4 events ────────────────────────────────────────────────────── */

static void l4_handler(struct net_mgmt_event_callback *cb,
		       uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (event == NET_EVENT_L4_CONNECTED) {
		s_net_connected = true;
		k_sem_give(&s_net_sem);
	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		s_net_connected = false;
	}
}

static void pub_queue_drain(void);

/* ── MQTT thread ──────────────────────────────────────────────────── */

static K_THREAD_STACK_DEFINE(s_stack, MQTT_THREAD_STACK);
static struct k_thread s_thread;

static void mqtt_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	net_mgmt_init_event_callback(&s_l4_cb, l4_handler,
				     NET_EVENT_L4_CONNECTED |
				     NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&s_l4_cb);

	uint32_t backoff_idx = 0;

	while (true) {
		if (!s_net_connected) {
			LOG_INF("MQTT thread: waiting for network...");
			k_sem_take(&s_net_sem, K_FOREVER);
		}

		/* connecting: broker_connect() sent MQTT CONNECT but CONNACK not
		 * yet received.  Prevents calling broker_connect() again on every
		 * loop iteration while we wait, which would leak TLS context slots
		 * and cause HiveMQ to forcibly drop the session (duplicate client_id). */
		bool connecting = false;

		while (s_net_connected) {
			if (!s_mqtt_connected && !connecting) {
				if (resolve_broker() != 0 ||
				    broker_connect() != 0) {
					uint32_t delay = s_backoff_sec[
						MIN(backoff_idx,
						    ARRAY_SIZE(s_backoff_sec)
						    - 1)];

					LOG_WRN("MQTT connect failed — "
						"retry in %u s", delay);
					k_sleep(K_SECONDS(delay));
					if (backoff_idx <
					    ARRAY_SIZE(s_backoff_sec) - 1) {
						backoff_idx++;
					}
					continue;
				}
				backoff_idx = 0;
				connecting = true;
			}

			/* Poll the socket for up to 1 s.  This guarantees
			 * mqtt_live() and pub_queue_drain() run at least once per
			 * second even when the broker sends nothing, so keepalive
			 * PINGREQ is sent on time and queued telemetry is published.
			 * mqtt_input() is only called when data is actually available,
			 * so it never blocks and there is no need for SO_RCVTIMEO. */
			struct zsock_pollfd pfd = {
				.fd     = s_client.transport.tls.sock,
				.events = ZSOCK_POLLIN,
			};
			int poll_rc = zsock_poll(&pfd, 1, 1000);

			if (poll_rc < 0) {
				LOG_WRN("poll error %d — reconnecting", poll_rc);
				k_mutex_lock(&s_mutex, K_FOREVER);
				s_mqtt_connected = false;
				k_mutex_unlock(&s_mutex);
				connecting = false;
				mqtt_disconnect(&s_client, NULL);
				continue;
			}

			if (poll_rc > 0) {
				int rc = mqtt_input(&s_client);

				if (rc && rc != -EAGAIN) {
					LOG_WRN("mqtt_input error %d — "
						"reconnecting", rc);
					k_mutex_lock(&s_mutex, K_FOREVER);
					s_mqtt_connected = false;
					k_mutex_unlock(&s_mutex);
					connecting = false;
					mqtt_disconnect(&s_client, NULL);
					continue;
				}
			}

			if (s_mqtt_connected) {
				connecting = false;
				pub_queue_drain();
				/* If pub_queue_drain() cleared s_mqtt_connected
				 * (mqtt_publish failed), close the socket before the
				 * next broker_connect() to avoid a duplicate-clientid
				 * forced disconnect from HiveMQ. */
				if (!s_mqtt_connected) {
					connecting = false;
					mqtt_disconnect(&s_client, NULL);
					continue;
				}
			}

			mqtt_live(&s_client);
		}

		if (s_mqtt_connected || connecting) {
			mqtt_disconnect(&s_client, NULL);
			k_mutex_lock(&s_mutex, K_FOREVER);
			s_mqtt_connected = false;
			k_mutex_unlock(&s_mutex);
		}
		backoff_idx = 0;
	}
}

/* ── Public API ───────────────────────────────────────────────────── */

int ringone_mqtt_init(void)
{
	derive_device_id();

	/* Strip optional ":port" from RINGONE_MQTT_BROKER_HOST.
	 * The Kconfig default may be written as "hostname:8883" for clarity;
	 * zsock_getaddrinfo and TLS SNI both need a bare hostname. */
	strncpy(s_broker_host, MQTT_BROKER_HOST, sizeof(s_broker_host) - 1);
	s_broker_host[sizeof(s_broker_host) - 1] = '\0';
	char *colon = strchr(s_broker_host, ':');
	if (colon) {
		*colon = '\0';
	}
	LOG_INF("MQTT broker: %s:%d", s_broker_host, MQTT_BROKER_PORT);

	/* Load credentials from PSA Protected Storage; fall back to Kconfig */
	size_t ulen = 0, plen = 0;
	psa_status_t ps_err = psa_ps_get(PS_UID_MQTT_USERNAME, 0,
					 sizeof(s_mqtt_user_buf) - 1,
					 s_mqtt_user_buf, &ulen);
	if (ps_err == PSA_SUCCESS && ulen > 0) {
		s_mqtt_user_buf[ulen] = '\0';
		psa_ps_get(PS_UID_MQTT_PASSWORD, 0,
			   sizeof(s_mqtt_pass_buf) - 1,
			   s_mqtt_pass_buf, &plen);
		s_mqtt_pass_buf[plen] = '\0';
		LOG_INF("MQTT credentials loaded from Protected Storage");
	} else {
		strncpy(s_mqtt_user_buf, CONFIG_RINGONE_MQTT_USERNAME,
			sizeof(s_mqtt_user_buf) - 1);
		strncpy(s_mqtt_pass_buf, CONFIG_RINGONE_MQTT_PASSWORD,
			sizeof(s_mqtt_pass_buf) - 1);
		ulen = strlen(s_mqtt_user_buf);
		plen = strlen(s_mqtt_pass_buf);
		if (ulen > 0) {
			LOG_WRN("MQTT credentials from Kconfig defaults "
				"(dev mode)");
		}
	}

	s_mqtt_user = (struct mqtt_utf8){.utf8 = s_mqtt_user_buf, .size = ulen};
	s_mqtt_pass = (struct mqtt_utf8){.utf8 = s_mqtt_pass_buf, .size = plen};

	k_thread_create(&s_thread, s_stack,
			K_THREAD_STACK_SIZEOF(s_stack),
			mqtt_thread_fn, NULL, NULL, NULL,
			MQTT_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&s_thread, "mqtt_client");
	return 0;
}

void mqtt_publish_telemetry(const ringone_data_t *data)
{
	if (!data) {
		return;
	}
	/* Non-blocking enqueue — mqtt_thread drains this and calls
	 * mqtt_publish() in its own context, keeping TLS encrypt off sysworkq. */
	k_msgq_put(&s_pub_queue, data, K_NO_WAIT);
}

static void pub_queue_drain(void)
{
	ringone_data_t data;

	if (k_msgq_get(&s_pub_queue, &data, K_NO_WAIT) != 0) {
		return;
	}

	uint32_t ts = sntp_get_unix_time();
	char json[256];
	int jlen = snprintf(json, sizeof(json),
		"{\"device_id\":\"%s\","
		"\"ts\":%u,"
		"\"hr\":%u,"
		"\"spo2\":%u,"
		"\"temp\":%.2f,"
		"\"steps\":%u,"
		"\"battery\":%u,"
		"\"rssi_ble\":%d}",
		s_device_id, ts,
		(unsigned)data.heart_rate,
		(unsigned)data.spo2,
		(double)data.temperature / 100.0,
		(unsigned)data.steps,
		(unsigned)data.battery,
		-60);  /* RING_ONE_TODO: bt_conn_get_info() RSSI */

	if (jlen < 0 || jlen >= (int)sizeof(json)) {
		return;
	}

	char topic[64];

	snprintf(topic, sizeof(topic), "ring-one/%s/telemetry", s_device_id);

	struct mqtt_publish_param pub = {
		.message = {
			.topic = {
				.topic = {.utf8 = topic,
					  .size = strlen(topic)},
				.qos = MQTT_QOS_0_AT_MOST_ONCE,
			},
			.payload = {.data = json, .len = (uint32_t)jlen},
		},
		.message_id  = 0,
		.dup_flag    = 0,
		.retain_flag = 0,
	};

	int err = mqtt_publish(&s_client, &pub);

	if (err) {
		LOG_ERR("mqtt_publish failed (err %d)", err);
		k_mutex_lock(&s_mutex, K_FOREVER);
		s_mqtt_connected = false;
		k_mutex_unlock(&s_mutex);
	}
}

bool mqtt_client_connected(void)
{
	k_mutex_lock(&s_mutex, K_FOREVER);
	bool c = s_mqtt_connected;
	k_mutex_unlock(&s_mutex);
	return c;
}
