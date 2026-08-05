/* Ring•One Firmware · Dotstar Systems · Apache 2.0 */

/*
 * PATH B — HTTPS direct to InfluxDB Cloud v3
 *
 * Posts telemetry in InfluxDB line protocol to:
 *   https://<RINGONE_INFLUX_HOST>/api/v2/write
 * Authorization: Token <influx_token from PSA Protected Storage>
 *
 * Runs in a dedicated thread ("influx_pub") so the BLE notify loop
 * is never blocked by TLS handshakes.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <psa/protected_storage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "influx_telemetry.h"
#include "sntp_sync.h"

LOG_MODULE_REGISTER(influx_telemetry, LOG_LEVEL_INF);

/* RING_ONE_TODO: install Let's Encrypt CA cert (ISRG Root X1) for TLS
 * verification of cloud.influxdata.com.  Use tls_cred_add() at first boot:
 *   tls_credential_add(INFLUX_TLS_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
 *                      ca_cert_pem, ca_cert_pem_len);
 * Until provisioned, peer verification is skipped (dev mode). */

/* do_post() reuses one TLS connection across posts instead of paying a
 * fresh TCP+TLS handshake per write (see s_sock) — that's what makes a
 * short RINGONE_INFLUX_INTERVAL_SEC viable at all. At longer intervals
 * (dialed back up for battery life) the server's idle keep-alive timeout
 * will likely close it between posts anyway, back to one handshake per
 * write. RING_ONE_TODO for that case: batch N buffered points into a
 * single POST issued once per TWT wake, instead of reconnecting on a
 * cadence shorter than the TWT service period. */

#define INFLUX_HOST   CONFIG_RINGONE_INFLUX_HOST
#define INFLUX_PORT   443

#define INFLUX_THREAD_STACK  8192
#define INFLUX_THREAD_PRIO   8
#define INFLUX_TIMEOUT_MS    10000

/* PSA Protected Storage UID for InfluxDB token */
#define PS_UID_INFLUX_TOKEN  ((psa_storage_uid_t)0x524E4946U)  /* "RNIF" */
#define INFLUX_TOKEN_MAX_LEN 128

/* Message queue: main loop → influx thread */
K_MSGQ_DEFINE(s_queue, sizeof(ringone_data_t), 4, 4);

static volatile bool s_influx_ok;
static char s_device_id[16];
static char s_token[INFLUX_TOKEN_MAX_LEN];

/* Persistent TLS socket, reused across posts — only touched by influx_thread_fn.
 * A fresh TCP+TLS handshake per write is the dominant cost of a PATH B post
 * (hundreds of ms on this hardware); reusing the connection turns a repost
 * into one HTTP request/response over an already-open channel, which is what
 * makes a short RINGONE_INFLUX_INTERVAL_SEC actually viable. -1 == closed. */
static int s_sock = -1;

/* L4 connectivity */
static struct net_mgmt_event_callback s_l4_cb;
static K_SEM_DEFINE(s_net_sem, 0, 1);
static volatile bool s_net_up;

static void l4_handler(struct net_mgmt_event_callback *cb,
		       uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);
	if (event == NET_EVENT_L4_CONNECTED) {
		s_net_up = true;
		k_sem_give(&s_net_sem);
	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		s_net_up = false;
		s_influx_ok = false;
	}
}

static void derive_device_id(void)
{
	bt_addr_le_t addr;
	size_t count = 1;

	bt_id_get(&addr, &count);
	/* Must match mqtt_client.c's derive_device_id() exactly — both paths
	 * derive from the same BLE address and land in the same InfluxDB
	 * bucket/measurement (telegraf.conf bridges MQTT in too). A mismatch
	 * here means Grafana groups one physical ring into two series. */
	snprintf(s_device_id, sizeof(s_device_id), "ring-one-%02X%02X",
		 addr.a.val[1], addr.a.val[0]);
}

/* ── HTTP/TLS POST ────────────────────────────────────────────────── */

static void influx_disconnect(void)
{
	if (s_sock >= 0) {
		zsock_close(s_sock);
		s_sock = -1;
	}
}

static int influx_connect(void)
{
	struct zsock_addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;
	char port_str[8];

	snprintf(port_str, sizeof(port_str), "%d", INFLUX_PORT);
	int err = zsock_getaddrinfo(INFLUX_HOST, port_str, &hints, &res);

	if (err) {
		LOG_ERR("DNS lookup %s failed (err %d)", INFLUX_HOST, err);
		return -EHOSTUNREACH;
	}

	int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);

	if (sock < 0) {
		LOG_ERR("socket() failed (err %d)", errno);
		zsock_freeaddrinfo(res);
		return -errno;
	}

	/* TLS peer verification disabled until CA cert is provisioned.
	 * Do NOT set TLS_SEC_TAG_LIST — Zephyr validates the tag even when
	 * peer_verify is NONE, which triggers "No TLS credential found". */
	int verify = TLS_PEER_VERIFY_NONE;

	zsock_setsockopt(sock, SOL_TLS, TLS_PEER_VERIFY,
			 &verify, sizeof(verify));
	zsock_setsockopt(sock, SOL_TLS, TLS_HOSTNAME,
			 INFLUX_HOST, strlen(INFLUX_HOST));

	/* Connect timeout via SO_RCVTIMEO on TLS socket is unreliable;
	 * use non-blocking connect with poll instead. */
	struct timeval tv = {.tv_sec = INFLUX_TIMEOUT_MS / 1000};

	zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	err = zsock_connect(sock, res->ai_addr, res->ai_addrlen);
	zsock_freeaddrinfo(res);

	if (err) {
		LOG_ERR("TLS connect to %s failed (err %d)", INFLUX_HOST, errno);
		zsock_close(sock);
		return -errno;
	}

	s_sock = sock;
	return 0;
}

/* Read one full HTTP/1.1 response off s_sock — status line, headers, and
 * (if Content-Length says there is one) body — and report the status code.
 *
 * This has to fully drain the response, not just grab whatever one recv()
 * returns: s_sock is reused for the next POST, and any response bytes left
 * unread here (extra headers, a body past the first packet) would be
 * misread as the start of the *next* response, corrupting it. That's
 * exactly what a single-shot recv() got away with before the connection
 * was made persistent — the socket was thrown away right after, taking any
 * unread trailing bytes with it.
 *
 * *should_reuse is set false when the response has no Content-Length to
 * bound it (nothing this fixed-shape API sends today, but nothing rules it
 * out) — without a definite length there's no safe way to know the socket
 * is left clean, so the caller closes it rather than risk reusing it. */
static int read_http_response(bool *should_reuse)
{
	char buf[512];
	size_t len = 0;
	int status = -1;
	long content_length = -1;
	char *header_end = NULL;

	*should_reuse = true;

	while (len < sizeof(buf) - 1 && !header_end) {
		int r = zsock_recv(s_sock, buf + len, sizeof(buf) - 1 - len, 0);

		if (r <= 0) {
			return -1;
		}
		len += (size_t)r;
		buf[len] = '\0';
		header_end = strstr(buf, "\r\n\r\n");
	}

	if (!header_end) {
		return -1;
	}

	sscanf(buf, "HTTP/%*d.%*d %d", &status);

	char *cl = strstr(buf, "Content-Length:");

	if (cl && cl < header_end) {
		content_length = strtol(cl + strlen("Content-Length:"), NULL, 10);
	}

	if (content_length < 0) {
		*should_reuse = false;
		return status;
	}

	size_t body_have = len - (size_t)((header_end + 4) - buf);

	while ((long)body_have < content_length) {
		int r = zsock_recv(s_sock, buf, sizeof(buf) - 1, 0);

		if (r <= 0) {
			return -1;
		}
		body_have += (size_t)r;
	}

	return status;
}

/* One POST attempt over s_sock, connecting first if it isn't already open.
 * Any I/O failure closes s_sock so the next attempt (do_post()'s retry, or
 * the following publish cycle) reconnects from scratch. */
static int do_post_once(const char *body, size_t body_len)
{
	if (s_sock < 0) {
		int err = influx_connect();

		if (err) {
			return err;
		}
	}

	char url[256];

	snprintf(url, sizeof(url),
		 "/api/v2/write?org=%s&bucket=%s&precision=s",
		 CONFIG_RINGONE_INFLUX_ORG, CONFIG_RINGONE_INFLUX_BUCKET);

	char hdr[512];
	int hdr_len = snprintf(hdr, sizeof(hdr),
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Authorization: Token %s\r\n"
		"Content-Type: text/plain; charset=utf-8\r\n"
		"Content-Length: %zu\r\n"
		"Connection: keep-alive\r\n"
		"\r\n",
		url, INFLUX_HOST, s_token, body_len);

	if (hdr_len < 0 || hdr_len >= (int)sizeof(hdr)) {
		return -EMSGSIZE;
	}

	if (zsock_send(s_sock, hdr, hdr_len, 0) < 0 ||
	    zsock_send(s_sock, body, body_len, 0) < 0) {
		LOG_WRN("InfluxDB: send failed (errno %d) — "
			"connection likely stale", errno);
		influx_disconnect();
		return -EIO;
	}

	bool should_reuse;
	int status = read_http_response(&should_reuse);

	if (status < 0) {
		LOG_WRN("InfluxDB: no response — connection likely stale");
		influx_disconnect();
		return -EIO;
	}

	if (!should_reuse) {
		influx_disconnect();
	}

	if (status == 204) {
		LOG_INF("InfluxDB write OK [%s]", s_device_id);
		return 0;
	}

	LOG_ERR("InfluxDB write failed: HTTP %d", status);
	return -EPROTO;
}

static int do_post(const char *body, size_t body_len)
{
	bool had_existing = (s_sock >= 0);
	int err = do_post_once(body, body_len);

	if (err && had_existing) {
		/* The one failure mode worth a same-cycle retry: we reused a
		 * connection that the server (or its idle keep-alive timeout)
		 * had actually already closed. A fresh connect() should be
		 * fast since the network path is presumably still good.
		 * Don't retry a cold-connect failure here — that's already a
		 * full DNS+TCP+TLS timeout, and the normal per-cycle loop
		 * will try again next interval. */
		err = do_post_once(body, body_len);
	}

	return err;
}

/* ── Thread ───────────────────────────────────────────────────────── */

static K_THREAD_STACK_DEFINE(s_stack, INFLUX_THREAD_STACK);
static struct k_thread s_thread;

static void influx_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	net_mgmt_init_event_callback(&s_l4_cb, l4_handler,
				     NET_EVENT_L4_CONNECTED |
				     NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&s_l4_cb);

	while (true) {
		if (!s_net_up) {
			k_sem_take(&s_net_sem, K_FOREVER);
		}

		ringone_data_t data;
		int err = k_msgq_get(&s_queue, &data, K_SECONDS(60));

		if (err == -EAGAIN) {
			/* No data in 60 s — just loop */
			continue;
		}

		if (!s_net_up ||
		    wifi_prov_get_status() != WIFI_STATUS_CONNECTED) {
			/* SoftAP mode fires NET_EVENT_L4_CONNECTED for its own
			 * DHCP-assigned IP — that's not internet connectivity.
			 * Only POST when the device is actually connected to an
			 * upstream AP and the provisioning state agrees. */
			continue;
		}

		if (s_token[0] == '\0') {
			LOG_DBG("InfluxDB token not set — skipping POST");
			continue;
		}

		/* Build InfluxDB line protocol */
		uint32_t ts = sntp_get_unix_time();
		char line[256];
		int line_len = snprintf(line, sizeof(line),
			"ring_telemetry,device_id=%s "
			"hr=%u,spo2=%u,temp=%.2f,"
			"steps=%u,battery=%u,rssi_ble=%d "
			"%u",
			s_device_id,
			(unsigned)data.heart_rate,
			(unsigned)data.spo2,
			(double)data.temperature / 100.0,
			(unsigned)data.steps,
			(unsigned)data.battery,
			-60,   /* RING_ONE_TODO: bt_conn_get_info() RSSI */
			ts);

		if (line_len < 0 || line_len >= (int)sizeof(line)) {
			LOG_ERR("Line protocol buffer overflow");
			continue;
		}

		err = do_post(line, (size_t)line_len);
		s_influx_ok = (err == 0);
	}
}

/* ── Public API ───────────────────────────────────────────────────── */

int influx_telemetry_init(void)
{
	derive_device_id();

	/* Load InfluxDB token from PSA Protected Storage */
	size_t token_len = 0;
	psa_status_t ps_err = psa_ps_get(PS_UID_INFLUX_TOKEN, 0,
					 sizeof(s_token) - 1,
					 s_token, &token_len);
	if (ps_err == PSA_SUCCESS && token_len > 0) {
		s_token[token_len] = '\0';
		LOG_INF("InfluxDB token loaded from Protected Storage");
	} else {
		s_token[0] = '\0';
		LOG_WRN("InfluxDB token not provisioned — writes will fail "
			"until 'ringone_cred set influx_token <TOKEN>' is run");
	}

	k_thread_create(&s_thread, s_stack,
			K_THREAD_STACK_SIZEOF(s_stack),
			influx_thread_fn, NULL, NULL, NULL,
			INFLUX_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&s_thread, "influx_pub");
	return 0;
}

void influx_telemetry_publish(const ringone_data_t *data)
{
	if (!data || wifi_prov_get_status() != WIFI_STATUS_CONNECTED) {
		return;
	}
	/* Non-blocking: drop if queue full */
	k_msgq_put(&s_queue, data, K_NO_WAIT);
}

bool influx_telemetry_connected(void)
{
	return s_influx_ok;
}
