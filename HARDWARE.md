# Stovyn Board — Hardware Reference

Pin map, component list, and the design notes behind them, for the Stovyn main-product board.
MCU: **ESP32-S3-WROOM-1 (N16R8 — 16 MB flash / 8 MB PSRAM)**. This is what the firmware in this
repo expects; the pin map below is authoritative (it is what `hejia_hwtest/hejia_hwtest.ino`
compiles against).

---

## 1. Required change and constraints (please read before layout)

- **PIR on GPIO5, not GPIO46.** GPIO46 cannot wake the S3 from deep sleep (only GPIO0–21 can),
  so the product would lose motion-wake. GPIO5 is free, RTC-capable, and non-strapping. This one
  change also frees GPIO46 to be the I2S microphone data line (see audio).
- **No native USB.** Camera D5/D6 sit on GPIO19/20, which are the USB D-/D+ pins. So there is no
  USB console/DFU: program and view the console over a **USB-UART bridge on GPIO43 (TX) / GPIO44
  (RX)**, and do field updates over **Bluetooth (BLE OTA)**.
- **Strapping pins** sampled at reset — give each a defined level so nothing holds them wrong at
  power-on: **GPIO0** (boot/button), **GPIO45** (camera HREF), **GPIO46** (mic data).

---

## 2. Pin map

| Function | GPIO | Notes |
|---|---:|---|
| WS2812 status LED | 48 | single addressable LED |
| Boot button | 0 | strapping; idle-high |
| PIR motion | 5 | moved from GPIO46; RTC-capable (deep-sleep wake) |
| **Sensor I2C** SDA / SCL | 38 / 4 | MLX90640 + TMP112 + BQ27441 |
| **Camera SCCB** SDA / SCL (SIOD/SIOC) | 39 / 40 | separate I2C bus from the sensors |
| Camera XCLK / PCLK | 41 / 42 | XCLK 20 MHz |
| Camera VSYNC / HREF | 7 / 45 | HREF is a strapping pin |
| Camera RESET / PWDN | 6 / 47 | |
| Camera data D0–D7 | 9, 10, 11, 12, 13, 19, 20, 21 | D5/D6 = GPIO19/20 = USB D-/D+ |
| **eMMC 4-bit** CLK / CMD | 14 / 15 | |
| eMMC D0–D3 | 16, 17, 18, 8 | |
| **I2S audio** BCLK / WS (LRC) | 1 / 2 | shared by speaker + mic (full-duplex) |
| I2S speaker data (DOUT) | 3 | to amp DIN (this pin is the piezo on boards without the amp) |
| I2S mic data (SD) | 46 | from the I2S MEMS mic; strapping pin |
| UART bridge TX / RX | 43 / 44 | CH340/CP2102 for flashing + console |

---

## 3. Components

### MCU
- **ESP32-S3-WROOM-1-N16R8** — 16 MB flash, 8 MB PSRAM (PSRAM is required: camera framebuffers +
  thermal streaming live in PSRAM).

### Sensor I2C bus (SDA 38 / SCL 4)
- **MLX90640** — 32×24 far-IR thermal array (the primary fire/heat sensor), address `0x33`.
- **TMP112** — ambient temperature, address `0x48`.
- **BQ27441** — battery fuel gauge, address `0x55`.
- **Pull-ups: ~1 kΩ, not the usual 4.7 kΩ**, with short routing. The gauge and TMP112 cap the bus
  at 400 kHz, but the firmware **bursts the MLX read to 1 MHz** (Fast-mode+) for smooth thermal
  streaming to the phone, then drops back to 400 kHz for the other two. 1 MHz needs the stronger
  pull-ups; the firmware auto-falls-back to 400 kHz if a given board can't hold the faster edges.

### Camera (DVP, on its own SCCB bus 39/40)
- **OV5640** — SCCB address `0x3C`. Provide the **2.8 V and 1.2 V** camera rails. 8-bit DVP data
  on the pins above. XCLK 20 MHz. (The board also carries an eMMC for image/clip storage.)

### Audio — one shared full-duplex I2S bus, 4 pins: BCLK 1, WS 2, speaker-out 3, mic-in 46

The speaker and the microphone share one I2S peripheral (that is why only 4 pins are needed).
Use an **I2S** microphone, **not PDM** — PDM would need its own 2 pins (5 total) and can't share
the bus.

**Amplifier (pick one):**
- Simplest — **MAX98357A** (e.g. `MAX98357AETE+`): I2S class-D, ~3.2 W, no configuration. Fine for
  notification chimes and alerts.
- **Recommended for phone-quality, "Samsung-style" sound — a smart amp** with boost + speaker
  protection: **TI TAS2563** or **ADI/Maxim MAX98390**. These drive a tiny speaker much louder and
  cleaner by boosting the rail and limiting cone excursion in real time (this is how phones get
  loud out of small drivers). They take I2S audio plus an I2C control line.

**Speaker:**
- SMT micro speaker, **8 Ω, ≥1 W**, e.g. **PUI Audio SMS-1508MS-R** (or an equivalent ~15 mm SMT
  part). It **needs a sealed rear acoustic volume** (back-cavity) in the enclosure — an unsealed
  speaker sounds thin and quiet no matter the amp.

**Microphone:**
- **I2S MEMS mic — InvenSense ICS-43434** (or **INMP441**), data on GPIO46. The mic is required:
  it powers **on-device whistle detection** (kettle / pressure-cooker), which the app already reads
  and surfaces. Give it a small acoustic port to the outside, placed away from the speaker to
  reduce coupling (the firmware also mutes detection during playback so the speaker can't
  self-trigger it).

> The **fire alarm stays a piercing tone by design** — only the non-emergency notifications use the
> pleasant chimes.

---

## 4. Enclosure notes

- **Speaker:** sealed back-volume behind the driver (a few cc) for loudness and low-end; a front
  port/grille in front of the cone.
- **Microphone:** a small acoustic port to outside air; keep it away from the speaker port.
- **Antenna:** keep metal and copper out of the WROOM module's antenna keep-out, or WiFi/BLE range
  suffers. The Android bench app has a **WiFi telemetry + latency/RSSI test** specifically to
  measure how much the assembled enclosure attenuates the radio — run it with the lid on.

---

Questions on any of this: **Augeas Technologies** — info@augeastechnologies.com
