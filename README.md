# Ring•One Firmware

Firmware for the Ring•One industrial smart ring reference platform.

**Platform:** Nordic nRF54LM20DK · **SDK:** nRF Connect SDK v3.3.0 · **BLE:** 5.4

Developed by [Dotstar Systems](https://dotstarsystems.com).

---

## Companion app

Android / iOS companion: [dotstar-consulting/ringone-app](https://github.com/dotstar-consulting/ringone-app)

---

## GATT service contract

| Role        | UUID                                 | Format                      |
|-------------|--------------------------------------|-----------------------------|
| Service     | fd0d5c94-193c-496e-b80f-511a474a449f | —                           |
| Temperature | ac70a713-348e-43db-bf84-ffce9d82120d | int16 LE · 0.01 °C / LSB    |
| Heart Rate  | 75fb4a26-440c-4dd3-be96-91ad75ecb864 | uint8 · BPM                 |
| SpO2        | c4671ec2-35f1-40c4-887b-37bc00ec3427 | uint8 · %                   |
| Steps       | 7956ed3f-1cb4-47ce-89ad-9742bc0ab8bf | uint32 LE · count since boot |
| Battery     | 02e35db9-662d-4229-a874-d4f04c82653a | uint8 · %                   |

All characteristics are **notify-only** (no read permission). The device advertises as `Ring-One`.

---

## Wi-Fi telemetry setup

### Hardware
Plug nRF7002 EB2 shield onto the nRF54LM20DK P1/P2 headers.

### Build with Wi-Fi (primary target)
```sh
west build -b nrf54lm20dk/nrf54lm20b/cpuapp app/ \
  --pristine -- -DSHIELD="nrf7002eb2;nrf7002eb2_coex"
```

The `nrf7002eb2_coex` shield adds the `nrf_radio_coex` DTS node which
auto-selects `MPSL_CX` and `MPSL_CX_NRF700X` — enabling BLE/Wi-Fi
coexistence without any manual Kconfig. Do **not** set those symbols manually.

### Provision Wi-Fi credentials — Option A: SoftAP (recommended)
On first boot with no stored credentials, the device starts a `ringone`
Wi-Fi hotspot running an HTTPS provisioning server. Connect your phone to
`ringone`, open a browser, enter your home AP credentials, and the device
reboots into station mode automatically.

### Provision Wi-Fi credentials — Option B: shell
Connect serial terminal (115200 baud), then:
```
uart:~$ wifi_cred add "YourSSID" WPA2-PSK "YourPassword"
uart:~$ kernel reboot warm
```

### Provision MQTT TLS certificate (HiveMQ Cloud)
1. Download HiveMQ Cloud root CA:
   https://letsencrypt.org/certs/isrgrootx1.pem
2. Install via TLS credentials shell:
   ```
   uart:~$ tls_cred add 1 ca_cert <paste PEM here>
   ```

### Cloud stack
| Component   | Service                                         |
|-------------|-------------------------------------------------|
| MQTT broker | HiveMQ Cloud (free tier — 100 connections)      |
| Time-series | InfluxDB Cloud v3 (free tier — 30 days)         |
| Dashboard   | Grafana Cloud (free tier — shareable URL)       |

HiveMQ → InfluxDB: Telegraf MQTT consumer plugin  
InfluxDB → Grafana: native InfluxDB data source plugin

### MQTT topics
| Topic                            | Direction | QoS | Interval |
|----------------------------------|-----------|-----|----------|
| `ring-one/<clientId>/telemetry`  | publish   | 0   | 30 s     |
| `ring-one/<clientId>/cmd`        | subscribe | 0   | future OTA |

### Telegraf config snippet
```toml
[[inputs.mqtt_consumer]]
  servers = ["ssl://your-cluster.s1.eu.hivemq.cloud:8883"]
  topics  = ["ring-one/+/telemetry"]
  data_format = "json"
  json_time_key = "ts"
  json_time_format = "unix"

[[outputs.influxdb_v2]]
  urls   = ["https://eu-central-1-1.aws.cloud2.influxdata.com"]
  token  = "$INFLUX_TOKEN"
  org    = "dotstar"
  bucket = "ringone"
```

### Local demo (no cloud accounts)

`local_cloud` branch, for demoing against [ringone-cloud-local](../ringone-cloud-local)
(self-hosted InfluxDB + Mosquitto + Grafana) instead of the live HiveMQ
Cloud / InfluxDB Cloud accounts above:

```sh
# In ringone-cloud-local: ./ringone-cloud start
# prints the LAN IP to use below.
west build -b nrf54lm20dk/nrf54lm20b/cpuapp app/ \
  -- -DSHIELD="nrf7002eb2;nrf7002eb2_coex" \
     -DEXTRA_CONF_FILE=local_cloud.conf \
     -DCONFIG_RINGONE_INFLUX_HOST=\"<lan-ip>\" \
     -DCONFIG_RINGONE_MQTT_BROKER_HOST=\"<lan-ip>:8883\"
```

Then provision the InfluxDB token over the device shell:
```
uart:~$ ringone_cred set influx_token ringone-local-dev-token
```

See `app/local_cloud.conf` for details — no MQTT credentials needed,
Mosquitto there allows anonymous connections.

---

## Building

### Prerequisites

- [nRF Connect SDK v3.3.0](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/3.3.0/)
- `west` ≥ 1.2 — `pip install west`
- Zephyr SDK ≥ 0.17.0 with ARM toolchain

### Set up west workspace

```sh
# Clone this repo, then from the parent directory:
west init -l ringone-firmware
west update
```

### Build

```sh
# Wi-Fi + BLE coexistence (primary):
west build -b nrf54lm20dk/nrf54lm20b/cpuapp ringone-firmware/app \
  -- -DSHIELD="nrf7002eb2;nrf7002eb2_coex"

# BLE-only (no Wi-Fi shield):
west build -b nrf54l15dk/nrf54l15/cpuapp ringone-firmware/app
```

### Flash

```sh
west flash
```

---

## Repository layout

```
ringone-firmware/
├── app/
│   ├── src/main.c          GATT server, BLE advertising, notify loop
│   ├── CMakeLists.txt
│   └── prj.conf
├── boards/                  Board overlays (future hardware bring-up)
├── drivers/sensors/
│   ├── ringone_sensors.h
│   └── ringone_sensors.c   Stub drivers — replace with real IC drivers
├── .github/workflows/
│   └── build.yml           CI: west build on push to main
└── west.yml                 NCS v3.3.0 manifest
```

### Stub sensors

All sensor functions are marked `RING_ONE_TODO` and return hard-coded values.
Replace with real drivers when hardware is available:

| Characteristic | Stub value | Suggested IC | Interface        |
|----------------|-----------|--------------|------------------|
| Temperature    | 25.69 °C  | MAX30205     | I2C `&i2c20` 0x48 |
| Heart Rate     | 72 BPM    | MAX30101     | I2C `&i2c20` 0x57 |
| SpO2           | 98 %      | MAX30101     | I2C `&i2c20` 0x57 |
| Steps          | counter++ | LSM6DSO      | I2C `&i2c20` 0x6A |
| Battery        | 85 %      | nRF SAADC    | ADC VBAT divider  |

---

## License

Apache 2.0 — see [LICENSE](LICENSE).  
Copyright (c) 2026 Dotstar Systems (dotstarsystems.com)
