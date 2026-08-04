# Ring•One Custom PCB — Technical Handoff Brief

Purpose: paste this into the other conversation (the one with the
"building a prototype product to showcase consulting abilities" business
context) so it can produce a single, ready-to-paste prompt for flux.ai.
This file only covers the technical/engineering side gathered from the
firmware repo and bring-up session — it does not know the business
framing, target users, or budget, which is why the open questions at the
bottom are left for that conversation to resolve.

## What exists today

Firmware repo: `ringone-firmware` (nRF Connect SDK v3.3.0, Zephyr).
Currently developed/tested on **nRF54LM20DK (SoC variant A)** +
**nRF7002 EB2** Wi-Fi companion shield.

- Wireless: BLE (peripheral, GATT) is native to the nRF54LM20. Wi-Fi is
  **not** native — it comes from an external nRF7002 companion chip over
  SPI, because the nRF54LM20 series is a BLE/multiprotocol low-power SoC,
  not a Wi-Fi SoC.
- Cloud telemetry: InfluxDB HTTPS POST, HiveMQ MQTT (TLS), SoftAP-based
  Wi-Fi provisioning, SNTP time sync.
- GATT contract (BLE, notify-only characteristics today):

  | Role        | Format                       |
  |-------------|------------------------------|
  | Temperature | int16 LE · 0.01 °C / LSB     |
  | Heart Rate  | uint8 · BPM                  |
  | SpO2        | uint8 · %                    |
  | Steps       | uint32 LE · count since boot |
  | Battery     | uint8 · %                    |

- Sensors wired up so far:
  - **MAX30101** (PPG — heart rate + SpO2), on I2C21, addr 0x57.
    RED+IR channels in `spo2` acquisition mode. Firmware does
    peak-detection HR + ratio-of-ratios SpO2 post-processing (uncalibrated
    linear approximation, needs a reference pulse oximeter to calibrate
    properly before shipping).
  - **LSM6DSO** (6-DoF IMU) — user has this part in hand, not yet wired
    into this board; original plan was I2C, addr 0x6A.
  - Temperature, steps, battery are currently **stubbed/hardcoded** in
    firmware pending real sensors. The original placeholder for skin
    temperature was MAX30205 — but there is **no mainline Zephyr driver**
    for that exact part, so it should be swapped (see below).

- Hard-won lesson from MAX30101 bring-up, worth carrying into PCB layout:
  the nRF7002 shield's BUCKEN (buck-converter enable) line landed on the
  same SoC pin (P1.04) originally picked for I2C SDA. Enabling the I2C
  peripheral on that pin re-muxed it away from plain GPIO, so the Wi-Fi
  radio could never power on — and this failure persisted even with the
  sensor physically unplugged, because the conflict was in the SoC pin
  mux, not the external part. **Any pin-map for a new PCB needs to be
  checked against every peripheral that shares a GPIO port/pin, including
  power-sequencing/enable lines for any companion radio — not just signal
  pins.** Also worth noting: even a devicetree-clean pin can still fail
  electrically in practice (P1.02 was clean in software but unreliable on
  the breadboard; P1.03 worked) — so pin choices should have a fallback
  candidate.

## What's being asked for now: a custom PCB

### SoC target: nRF54LM20 — variant B, not variant A

The DK on hand is variant A. The new board needs **variant B**, which
the user needs for EdgeAI capability. All pin validation done so far
(I2C21 SDA/SCL placement, BUCKEN conflict, etc.) was against variant A's
DK pinout — **variant B's exact package/pinout compatibility with
variant A has not been verified in this session** and should be
confirmed against Nordic's datasheet before finalizing the PCB footprint.

### Sensors: prioritize parts with existing mainline Zephyr drivers

Surveyed directly against this repo's `zephyr/drivers/sensor/` tree
(NCS v3.3.0) rather than guessed:

| Function                     | Candidate part                | Zephyr Kconfig            | Notes |
|-------------------------------|--------------------------------|----------------------------|-------|
| PPG — heart rate / SpO2       | MAX30101 (have)                 | `CONFIG_MAX30101`          | Already integrated and bring-up debugged in this repo |
| 6-DoF IMU                     | LSM6DSO (have)                  | `CONFIG_LSM6DSO`           | User already owns this part |
| Skin temperature              | STTS22H (ST) *or* MAX31875 (Maxim) | `CONFIG_STTS22H` / `CONFIG_MAX31875` | Replaces the MAX30205 placeholder, which has no Zephyr driver. STTS22H is extremely small/low-power and common in wearables specifically; MAX31875 is a reasonable Maxim-family alternative |
| Battery fuel gauge            | MAX17055 (or MAX17262)          | `CONFIG_MAX17055`          | Replaces today's hardcoded 85%/ADC-divider stub with real SoC%, voltage, current |
| Optional — wear/proximity detection | SX9500 (capacitive) *or* APDS9960 (optical) | `CONFIG_SX9500` / `CONFIG_APDS9960` | Nice-to-have: auto power-gate sampling when the ring isn't being worn. Not load-bearing for a v1 prototype |
| Optional — 9-axis / compass    | + LIS3MDL magnetometer          | `CONFIG_LIS3MDL`           | Only relevant if a compass/orientation feature is wanted; adds BOM + power cost, otherwise skip |

Steps/pedometer does **not** need a dedicated chip — it can be computed
in firmware from the LSM6DSO's accelerometer stream.

### Electrical: unify the IO voltage domain

Most modern low-power wearable sensor ICs above (STTS22H, LSM6DSO,
MAX17055) run natively at **1.8V IO**. MAX30101 and the nRF54LM20 GPIOs
can typically run 1.8–3.3V depending on strapping/regulator config. To
avoid a pile of per-chip level shifters on a space-constrained board, the
PCB should pick **one shared IO rail (recommend 1.8V)** and confirm every
part on the BOM tolerates it, rather than mixing domains.

### Form factor

Needs to be something an end user can actually test/wear as a
prototype — not just a dev-kit-shaped board. What form factor and power
architecture make sense depends on the business goals from the other
conversation (this session doesn't have that context), so it's an open
question below rather than a decision made here.

## Open questions (need the business-context conversation to resolve)

1. **Form factor**: true finger-ring (BLE-only on-ring, since the
   nRF54LM20 has no native Wi-Fi and fitting an nRF7002 companion +
   antenna on a ring is impractical; a phone app would relay to cloud)
   vs. a larger wearable puck/clip/band that keeps Wi-Fi on-device
   (matches the current firmware's direct-to-cloud architecture as-is)
   vs. a two-piece split (tiny BLE-only ring + separate charging/Wi-Fi
   dock it syncs through periodically).
2. **Power**: small rechargeable LiPo (~20-40mAh) with pogo-pin charging
   (realistic for an actual ring, needs a small Li-ion charge-management
   IC on the PCB) vs. a non-rechargeable coin cell (CR2016/CR2025 —
   simplest BOM, swap when flat, good for a quick throwaway demo unit)
   vs. a larger LiPo (100mAh+) with USB-C (only realistic for the
   puck/band form factor).
3. Whether the Wi-Fi/InfluxDB/MQTT cloud paths should run directly off
   the wearable at all given the size/power budget of an actual ring, or
   whether that's better relayed through a phone or base station.

## The ask

Please produce a single, ready-to-paste prompt for flux.ai to generate
this custom PCB — resolving the three open questions above using the
"prototype to showcase consulting abilities" context from our other
conversation, and folding in the technical constraints from this brief
(variant B verification, the sensor table + exact Zephyr Kconfig names,
the unified 1.8V IO rail, and the pin-conflict lesson re: companion-radio
power-sequencing lines).
