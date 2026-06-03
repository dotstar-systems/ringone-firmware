# Ring•One Cloud Setup

## Stack

```
Device → HTTPS → InfluxDB Cloud v3      (primary telemetry)
Device → MQTT/TLS → HiveMQ Cloud        (bidirectional commands)
InfluxDB → Grafana Cloud                (dashboard)
Supabase                                (device registry: device_id → user_id)
```

## Why two protocols

**HTTPS to InfluxDB**: simpler, no broker, battery-efficient with TLS session
resumption.  Best for periodic time-series telemetry.  Zero broker dependency.

**MQTT to HiveMQ**: bidirectional.  The device can receive commands (OTA trigger,
config update, ping) without polling.  MQTT wins when the cloud needs to push to
the device.

This is the production pattern.  Not either/or.

---

## InfluxDB setup

1. Create account at [cloud.influxdata.com](https://cloud.influxdata.com) (free tier)
2. Create org: `dotstar`
3. Create bucket: `ringone` (recommended: 30 day retention)
4. Create API token — **write-only** scope on the `ringone` bucket
5. Provision the token to the device via UART shell:

```
uart:~$ ringone_cred set influx_token <YOUR_TOKEN>
```

The token is stored in PSA Protected Storage (CRACEN HUK-encrypted, never
plaintext in flash).  The device reads it at boot from `psa_ps_get()`.

### Line protocol example

```
ring_telemetry,device_id=rng-A3F2 hr=72,spo2=98,temp=25.69,steps=1240,battery=85,rssi_ble=-62 1748870400
```

### InfluxDB write endpoint

```
POST https://eu-central-1-1.aws.cloud2.influxdata.com/api/v2/write
  ?org=dotstar&bucket=ringone&precision=s
Authorization: Token <influx_token>
Content-Type: text/plain; charset=utf-8
```

---

## HiveMQ setup

1. Create account at [hivemq.com](https://www.hivemq.com) (free serverless tier)
2. Note your cluster hostname: `<cluster-id>.s1.<region>.hivemq.cloud`
3. Create MQTT credentials (username + password) in the HiveMQ Cloud console
4. Download the HiveMQ Cloud CA certificate (Let's Encrypt ISRG Root X1 chain)
5. Provision credentials to the device:

```
uart:~$ tls_cred add 1 ca_cert <paste PEM here>
uart:~$ ringone_cred set mqtt_user <username>
uart:~$ ringone_cred set mqtt_pass <password>
```

### Topic scheme

| Topic | Direction | QoS | Purpose |
|-------|-----------|-----|---------|
| `ring-one/<device_id>/telemetry` | Device → Cloud | 0 | Sensor JSON every `TELEMETRY_INTERVAL_SEC` |
| `ring-one/<device_id>/cmd` | Cloud → Device | 1 | Commands (ping, reboot, set_interval, get_status) |
| `ring-one/<device_id>/status` | Device → Cloud | 0 | Responses to get_status / pong |

### Commands

```json
{"cmd": "ping"}
{"cmd": "reboot"}
{"cmd": "set_interval", "value": 60}
{"cmd": "get_status"}
```

---

## Telegraf on Fly.io (MQTT → InfluxDB bridge, optional)

Provides an alternative ingestion path via MQTT when direct HTTPS is unavailable
or for debugging the MQTT path.

### fly.toml

```toml
app = "ringone-telegraf"

[build]
  image = "telegraf:1.30"

[env]
  HIVEMQ_HOST = "your-cluster.s1.eu.hivemq.cloud"
  INFLUX_HOST = "https://eu-central-1-1.aws.cloud2.influxdata.com"
```

### telegraf.conf

```toml
[[inputs.mqtt_consumer]]
  servers       = ["ssl://${HIVEMQ_HOST}:8883"]
  topics        = ["ring-one/+/telemetry"]
  username      = "${HIVEMQ_USER}"
  password      = "${HIVEMQ_PASS}"
  data_format   = "json"
  json_time_key = "ts"
  json_time_format = "unix"

[[outputs.influxdb_v2]]
  urls         = ["${INFLUX_HOST}"]
  token        = "${INFLUX_TOKEN}"
  organization = "dotstar"
  bucket       = "ringone"
```

Deploy:

```
fly launch
fly secrets set HIVEMQ_USER=... HIVEMQ_PASS=... INFLUX_TOKEN=...
fly deploy
```

Cost: free tier (shared CPU, 256 MB RAM — sufficient for Telegraf).

---

## Supabase device registry

Maps `device_id` (`rng-XXXX`) → `user_id` for multi-device fleet management.
Schema: see [`docs/supabase-schema.sql`](supabase-schema.sql).

```
supabase db push
```

---

## Grafana dashboard

1. Add InfluxDB Cloud as a data source (Flux query language)
2. Import dashboard JSON from `docs/grafana-dashboard.json`
3. Set variable: `$device_id` (from Supabase SQL via PostgreSQL data source)
4. Panels: HR trend, SpO2, Temperature, Steps, Battery SoC, BLE RSSI

### Example Flux query

```flux
from(bucket: "ringone")
  |> range(start: -24h)
  |> filter(fn: (r) => r._measurement == "ring_telemetry")
  |> filter(fn: (r) => r.device_id == "${device_id}")
  |> filter(fn: (r) => r._field == "hr")
  |> aggregateWindow(every: 1m, fn: mean)
```

---

## Wi-Fi provisioning

### Method A — BLE-assisted (preferred)

1. Open the Ring-One companion app
2. App discovers the ring via BLE and connects
3. App pairs (bonded connection required for password write)
4. App writes `WiFi-SSID` characteristic (`a1b2c3d4-...-0001`)
5. App writes `WiFi-Password` characteristic (`a1b2c3d4-...-0002`) — requires bond
6. Device connects and notifies `WiFi-Status` (`a1b2c3d4-...-0003`):
   - `0x01` = connecting
   - `0x02` = connected (success)
   - `0x03` = failed

### Method B — SoftAP captive portal (fallback)

1. Long-press Button 1 on the DK for 5 seconds
2. Device broadcasts `RingOne-Setup` Wi-Fi network (open, no password)
3. Connect your phone to `RingOne-Setup`
4. The NCS SoftAP provisioning library serves a provisioning page
5. Enter home Wi-Fi SSID and password
6. Device connects and shuts down the SoftAP

### Credential storage

Credentials are stored via `wifi_credentials_set_personal()` in Zephyr's
Wi-Fi credentials subsystem (backed by ZMS flash storage).  They survive
reboots and power cycles.

---

## TF-M secure build (future work)

The `build-wifi-secure` CI job is a placeholder.  To enable TF-M:

1. Add to `prj.conf`:
   ```
   CONFIG_BUILD_WITH_TFM=y
   CONFIG_TFM_PROFILE_TYPE_SMALL=y
   CONFIG_PM_PARTITION_SIZE_TFM=0x20000
   CONFIG_PM_PARTITION_SIZE_TFM_SRAM=0xa000
   ```
2. Create `pm_static.yml` defining the Secure/Non-Secure partition split
3. Build target: `nrf54lm20dk/nrf54lm20a/cpuapp/ns`
4. PSA Protected Storage (already used for InfluxDB token and MQTT creds)
   will be backed by the TF-M ITS (Internal Trusted Storage) API
