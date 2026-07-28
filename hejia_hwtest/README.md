# Stovyn Board Bring-Up + BLE OTA — Hejia Guide

One-page guide to flashing and updating the Stovyn main-product board
(`hejia_hwtest` firmware). Target MCU: **ESP32-S3 (N16R8, 16 MB flash / 8 MB PSRAM)**.

---

## 0. Pin notes before you build the PCB

The firmware matches `Pinouts.xlsx` with **one required change** and two constraints:

- **PIR must be on GPIO5, not GPIO46.** GPIO46 cannot wake the chip from deep sleep
  (only GPIO0-21 can on the S3), so the product would lose motion-wake. The firmware
  now expects PIR on **GPIO5** (free, RTC-capable, non-strapping). Please wire it there.
- **No native USB on this board.** Camera D5/D6 sit on GPIO19/20, which are the USB
  D-/D+ lines. So you program and view the console through a **USB-UART bridge (CH340 or
  CP2102) on GPIO43 (TX) / GPIO44 (RX)**. Field updates go over Bluetooth (below).
- **Strapping pins:** GPIO45 (camera HREF) and GPIO46 (unused now) are sampled at reset.
  Keep a defined pull so nothing holds them at the wrong level during power-on.

---

## 1. Flash the test firmware (over the UART bridge)

Install once: Arduino ESP32 core 3.x + `arduino-cli`.

```
# from the repo root
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default,USBMode=hwcdc,UploadSpeed=921600,CPUFreq=240 \
  --output-dir ./build hejia_hwtest

# upload: COMx = the CH340/CP2102 bridge port
arduino-cli upload -p COMx \
  --fqbn esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default \
  --input-dir ./build hejia_hwtest
```

If auto-reset is not wired, hold **BOOT**, tap **RESET**, release BOOT, then upload.
`CDCOnBoot=default` routes the console to UART0 so the bridge carries it.

---

## 2. Read the self-test result

On boot the board tests every peripheral and reports three ways:

- **WS2812 LED (GPIO48):** purple = testing, **green = all passed**, **red = a failure**,
  slow **blue pulse = done, BLE ready**.
- **Serial @ 115200** on the UART bridge — one line per peripheral (buzzer, button, PIR,
  MLX90640 / TMP112 / BQ27441 on the sensor I2C bus, camera init + one frame, eMMC mount).
- **NVS namespace `hwtest`** (read with esptool if no serial is attached): `ok`, `mlx`,
  `tmp`, `bq`, `cam`, `sd`, `camerr`, `frame`.

Expected on a good board: `RESULT: ALL CORE PERIPHERALS PASS` + solid green LED + a chirp.

---

## 3. Update the firmware over Bluetooth (BLE OTA)

Because there is no USB, you can reflash wirelessly. The board advertises as
**`Stovyn-HWTEST`** once the self-test finishes (blue pulsing LED).

```
pip install bleak
python hejia_hwtest/ble_ota.py ./build/hejia_hwtest.ino.bin
```

The script finds the board, uploads the image to its spare OTA slot, and the board
verifies it and reboots into it. A bad or interrupted upload is safe — the boot slot
only switches after a verified finish, otherwise the board keeps the current image.
(Any BLE tool such as nRF Connect works too: service `e7c1b100-…`, write
`01`+`<uint32 size LE>` to the CONTROL char, stream the image to the DATA char, then
write `02` to CONTROL. Watch the STATUS char for progress.)

---

## Quick troubleshooting

| Symptom | Likely cause |
|---|---|
| Red LED, `MLX/TMP/BQ MISSING` | sensor I2C (SDA GPIO38 / SCL GPIO4) not connected or wrong address |
| Red LED, `camera init FAILED` | camera ribbon / XCLK(41) / SCCB(39,40) wiring; check 2.8V + 1.2V rails |
| Red LED, `eMMC mount FAILED` | eMMC lines (CLK14/CMD15/D0-3=16,17,18,8) or pull-ups |
| No serial output | using `CDCOnBoot=cdc` by mistake, or bridge not on GPIO43/44 |
| Board not found over BLE | still running self-test, or too far — LED should be pulsing blue |
