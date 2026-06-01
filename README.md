# Ring•One Firmware

Firmware for the Ring•One industrial smart ring reference platform.

**Platform:** Nordic nRF54L15 DK · **SDK:** nRF Connect SDK v3.3.0 · **BLE:** 5.4

Developed by [Dotstar Systems and Consulting](https://dotstarconsulting.com).

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
Copyright (c) 2026 Dotstar Systems and Consulting (dotstarconsulting.com)
