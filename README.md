# Stovyn Board — Bring-Up & Diagnostics (Hejia)

Everything needed to bring up, verify, and wirelessly update the Stovyn main-product
board. Target MCU: **ESP32-S3-WROOM-1 (N16R8 — 16 MB flash / 8 MB PSRAM)**.

This repository has three parts:

| | What | Where |
|---|---|---|
| 1 | **Hardware reference** — pin map, component list (camera, mic, speaker/amp, sensors), and the design notes behind them | [`HARDWARE.md`](HARDWARE.md) |
| 2 | **Bring-up test firmware** — flashes to a fresh board and checks every peripheral, then advertises for wireless (BLE) updates | [`hejia_hwtest/`](hejia_hwtest/) |
| 3 | **Prebuilt images** — ready to flash, no toolchain needed | [`prebuilt/`](prebuilt/) |
| 4 | **Android bench app** — reads all telemetry over Bluetooth, pushes firmware over BLE, WiFi latency test, buzzer + mic tools | [`app/`](app/) + below |

---

## Android bench app (Stovyn Bench)

A single-screen Android tool that connects to the board over Bluetooth and dumps every
characteristic (full telemetry), pushes firmware over BLE, runs a WiFi telemetry + latency
check, plays the buzzer chimes/tones, and measures the real acoustic output with the phone
mic (FFT + live spectrum).

**Download:**
- APK: [`app/StovynBench-1.3.0.apk`](app/StovynBench-1.3.0.apk)
- ZIP (use this if your browser blocks the .apk): [`app/StovynBench-1.3.0.zip`](app/StovynBench-1.3.0.zip) — unzip to get the .apk
- Or the release page: https://github.com/AugeasTechnologies/stovyn-ota/releases/tag/bench-v1.3.0

Most browsers flag a direct .apk as unsafe ("can't be downloaded securely" / "this file may
harm your device") — that is normal for any APK; choose **Keep** / **Download anyway**. If your
browser won't allow it at all, download the **.zip** instead and unzip it. To install, allow your
browser (or Files app) to "Install unknown apps" in Android Settings, then open the .apk. The app
self-updates after that, so this is a one-time sideload.

---

## Quick start

### 1. Pin notes before you build the PCB

The firmware matches `Pinouts.xlsx` with **one required change** and two constraints:

- **PIR must be on GPIO5, not GPIO46.** GPIO46 cannot wake the chip from deep sleep (only
  GPIO0–21 can on the S3), so the product would lose motion-wake. Wire PIR to **GPIO5**
  (free, RTC-capable, non-strapping).
- **No native USB on this board.** Camera D5/D6 sit on GPIO19/20, which are the USB D-/D+
  lines. Program and view the console through a **USB-UART bridge (CH340 or CP2102) on
  GPIO43 (TX) / GPIO44 (RX)**. Field updates go over Bluetooth (step 3).
- **Strapping pins:** GPIO45 (camera HREF) and GPIO46 are sampled at reset — keep a defined
  pull so nothing holds them at the wrong level during power-on.

### 2. Flash the test firmware

**Easiest — prebuilt image, no toolchain** (via the UART bridge, with `esptool`):

```
esptool --chip esp32s3 -p COMx write_flash 0x0 prebuilt/hejia_hwtest-piezo-merged.bin
```

Use `-piezo-merged` for a board without the I2S speaker/mic, or `-speaker-mic-merged` once
those parts are populated. See [`prebuilt/README.md`](prebuilt/README.md).

**Or build from source** (Arduino ESP32 core 3.x + `arduino-cli`): see
[`hejia_hwtest/README.md`](hejia_hwtest/README.md).

### 3. Read the result

On boot the board tests every peripheral and shows the result three ways:

- **WS2812 LED (GPIO48):** purple = testing, **green = all passed**, **red = a failure**,
  slow **blue pulse = done, ready for BLE update**.
- **Serial @ 115200** on the UART bridge — one line per peripheral.
- **NVS namespace `hwtest`** — readable later even with no serial attached.

### 4. Update over Bluetooth (no USB needed)

The board advertises as **`Stovyn-HWTEST`** once the self-test finishes. Update it from the
Android bench app (Firmware OTA button), or from a computer:

```
pip install bleak
python hejia_hwtest/ble_ota.py prebuilt/hejia_hwtest-piezo.bin
```

A bad or interrupted upload is safe — the board only switches to the new image after a
verified finish, otherwise it keeps the current one.

---

## Support

Questions on the board bring-up: **Augeas Technologies** — info@augeastechnologies.com
