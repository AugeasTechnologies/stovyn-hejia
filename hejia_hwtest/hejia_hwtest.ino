// hejia_hwtest.ino — Stovyn main-product board bring-up test (for Hejia).
// ============================================================================
// Flash this to a freshly-assembled Hejia PCB to verify every peripheral is
// wired and alive on the Pinouts.xlsx (2026-07-27) pin map, and to update the
// board's firmware over Bluetooth (BLE OTA) — which matters on this board
// because the camera occupies the native-USB pins (GPIO19/20), so there is no
// USB console/DFU in the field.
//
// WHAT IT TESTS (results on Serial @115200 over the UART0 bridge, on the WS2812
// LED, and persisted to NVS "hwtest" so they can be read back with esptool even
// if no serial is attached):
//   1. WS2812 status LED   (GPIO48)          - visual
//   2. Piezo buzzer        (GPIO3)           - audible chirp
//   3. Push button         (GPIO0)           - reads idle-high
//   4. PIR motion          (GPIO5)           - reads the line
//   5. Sensor I2C bus       SDA38/SCL4        - pings MLX90640(0x33), TMP112(0x48), BQ27441(0x55)
//   6. Camera SCCB + init   SIOD39/SIOC40 ... - probes OV5640(0x3C) then esp_camera_init + 1 frame
//   7. eMMC 4-bit           CLK14/CMD15/D0-3  - mounts and reads card info
//
// LED PROTOCOL (WS2812 on GPIO48, via the core's built-in neopixelWrite):
//   purple flicker  -> booting / running tests
//   green           -> ALL peripherals passed
//   red             -> at least one FAILED (see Serial / NVS for which)
//   slow blue pulse -> tests done, BLE OTA advertising (ready to update)
//
// BLE OTA: advertises as "Stovyn-HWTEST". A phone/host writes the image to the
// DATA characteristic in order, bracketed by BEGIN/END on the CONTROL
// characteristic; on END the image is verified, set bootable, and the board
// reboots into it. See the protocol constants below. Uses the ESP-IDF A/B OTA
// machinery, so a bad/incomplete image never bricks the board (it just doesn't
// switch the boot slot).
//
// PROCESSOR-CONFLICT NOTES for this pin map (also see board_config.h HEJIA):
//   * PIR is on GPIO5 (moved from the Excel's GPIO46, which is NOT RTC-capable and
//     could not wake deep sleep). GPIO5 is RTC-capable, so the product firmware can
//     motion-wake on it. The board MUST wire PIR to GPIO5.
//   * Camera D5/D6 = GPIO19/20 = native USB D-/D+  -> no USB console; that is
//     exactly why this firmware carries BLE OTA.
//   * Camera HREF=GPIO45 and PIR=GPIO46 are strapping pins.
//
// BUILD (Arduino/arduino-cli). OTA-capable partition scheme REQUIRED (two app slots):
//   --fqbn esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=default
// Optional parts (append; both default OFF): --build-property "compiler.cpp.extra_flags=-DUSE_I2S_SPEAKER=1 -DUSE_I2S_MIC=1"
// (Verified: piezo 778 KB / speaker+mic 802 KB, both 25% of the 3 MB slot.)
// ============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include "esp_camera.h"
#include "esp_ota_ops.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Optional I2S audio: speaker (MAX98357A/smart amp on GPIO1/2/3) and/or I2S mic (ICS-43434 on
// GPIO46), sharing one full-duplex I2S bus. Set the flags to 1 for whichever parts the board has —
// the self-test then plays a chime through the speaker and/or reports the mic level + dominant tone.
#ifndef USE_I2S_SPEAKER
#define USE_I2S_SPEAKER 0
#endif
#ifndef USE_I2S_MIC
#define USE_I2S_MIC 0
#endif
#if USE_I2S_SPEAKER || USE_I2S_MIC
#include <ESP_I2S.h>
#include <math.h>
#endif

// ─── Pin map (Pinouts.xlsx 2026-07-27) — MUST match board_config.h HEJIA ─────
#define P_LED_WS2812   48
#define P_BUZZER        3
#define P_BUTTON        0
#define P_PIR           5     // moved from Excel's GPIO46 -> GPIO5 (RTC-capable, so product fw can deep-sleep-wake on it)

#define P_I2C_SDA      38     // sensor bus (MLX90640 + TMP112 + BQ27441)
#define P_I2C_SCL       4

#define P_CAM_SIOD     39     // camera SCCB SDA
#define P_CAM_SIOC     40     // camera SCCB SCL
#define P_CAM_XCLK     41
#define P_CAM_PCLK     42
#define P_CAM_VSYNC     7
#define P_CAM_HREF     45
#define P_CAM_RESET     6
#define P_CAM_PWDN     47
#define P_CAM_D0        9
#define P_CAM_D1       10
#define P_CAM_D2       11
#define P_CAM_D3       12
#define P_CAM_D4       13
#define P_CAM_D5       19     // = USB D-
#define P_CAM_D6       20     // = USB D+
#define P_CAM_D7       21

#define P_SD_CLK       14
#define P_SD_CMD       15
#define P_SD_D0        16
#define P_SD_D1        17
#define P_SD_D2        18
#define P_SD_D3         8

// I2C addresses we expect on the sensor bus
#define ADDR_MLX90640  0x33
#define ADDR_TMP112    0x48
#define ADDR_BQ27441   0x55
#define ADDR_OV5640    0x3C   // on the camera SCCB bus

// ── Optional I2S audio: speaker (TX) + mic (RX) on ONE shared full-duplex bus ─
#if USE_I2S_SPEAKER || USE_I2S_MIC
#define P_I2S_BCLK 1
#define P_I2S_LRC  2
#define P_I2S_DOUT 3     // ESP data out -> speaker amp DIN
#define P_I2S_MIC  46    // ESP data in  <- I2S mic SD
static I2SClass g_i2s;
static bool g_audioReady = false;
static void hwtestAudioInit() {
  if (g_audioReady) return;
  int dout = USE_I2S_SPEAKER ? P_I2S_DOUT : -1;
  int din  = USE_I2S_MIC     ? P_I2S_MIC  : -1;
  g_i2s.setPins(P_I2S_BCLK, P_I2S_LRC, dout, din, -1);
  g_audioReady = g_i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  Serial.printf("[TEST] I2S %s (BCLK1/WS2/DOUT%d/DIN%d)\n", g_audioReady ? "ready" : "FAILED", dout, din);
}
#endif
#if USE_I2S_SPEAKER
static void audioNote(float freq, int ms, float vol) {
  if (!g_audioReady) return;
  const int RATE = 16000; const int N = RATE * ms / 1000; int16_t buf[256];
  float phase = 0, dp = 2.0f * (float)M_PI * freq / RATE; int i = 0;
  while (i < N) {
    int c = (N - i) < 256 ? (N - i) : 256;
    for (int k = 0; k < c; k++) {
      float t = (float)(i + k) / N; float atk = t < 0.06f ? t / 0.06f : 1.0f; float dec = 1.0f - t;
      buf[k] = (int16_t)(sinf(phase) * 28000.0f * vol * atk * dec * dec);
      phase += dp; if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    }
    g_i2s.write((uint8_t*)buf, c * 2); i += c;
  }
}
static void hwtestSpeakerChime() {
  hwtestAudioInit();
  Serial.println(g_audioReady ? "[TEST] I2S speaker chime" : "[TEST] I2S speaker FAILED");
  if (g_audioReady) { audioNote(523, 150, 0.9f); audioNote(659, 150, 0.9f); audioNote(784, 300, 0.9f); }
}
#endif
#if USE_I2S_MIC
static void hwtestMicLevel() {
  hwtestAudioInit();
  if (!g_audioReady) { Serial.println("[TEST] I2S mic FAILED (init)"); return; }
  int16_t buf[512]; long peak = 0; double sumsq = 0; long nn = 0;
  for (int b = 0; b < 6; b++) {                       // ~6 x 32 ms blocks
    size_t got = g_i2s.readBytes((char*)buf, sizeof(buf));
    int samples = (int)(got / 2);
    for (int i = 0; i < samples; i++) { long a = labs(buf[i]); if (a > peak) peak = a; sumsq += (double)buf[i] * buf[i]; nn++; }
  }
  double rms = nn ? sqrt(sumsq / nn) : 0;
  Serial.printf("[TEST] I2S mic: peak=%ld rms=%.0f (n=%ld) — tap/whistle near it, rms should rise\n", peak, rms, nn);
}
#endif

// ─── BLE OTA UUIDs + protocol ───────────────────────────────────────────────
#define OTA_SVC_UUID   "e7c1b100-2f3a-4b6d-9c11-000000000001"
#define OTA_CTRL_UUID  "e7c1b101-2f3a-4b6d-9c11-000000000001"   // write: commands
#define OTA_DATA_UUID  "e7c1b102-2f3a-4b6d-9c11-000000000001"   // write-no-rsp: image bytes
#define OTA_STAT_UUID  "e7c1b103-2f3a-4b6d-9c11-000000000001"   // notify: status/progress
// CONTROL opcodes (first byte)
#define OTA_OP_BEGIN   0x01    // + uint32 LE total size
#define OTA_OP_END     0x02    // finalize + reboot
#define OTA_OP_ABORT   0x03

static Preferences nvs;

// ─── WS2812 status LED (uses the ESP32 core's built-in neopixelWrite) ────────
static void led(uint8_t r, uint8_t g, uint8_t b) {
#if P_LED_WS2812 >= 0
  neopixelWrite(P_LED_WS2812, r, g, b);
#endif
}

// ─── Test result bookkeeping ─────────────────────────────────────────────────
struct Results {
  bool i2c_mlx=false, i2c_tmp=false, i2c_bq=false;
  bool cam_sccb=false, cam_init=false, cam_frame=false;
  bool sd_mount=false;
  bool button_idle_high=false;
  int  pir_level=-1;
  uint32_t cam_err=0, frame_bytes=0;
  uint64_t sd_total_mb=0;
} R;

static bool i2cPing(TwoWire& bus, uint8_t addr) {
  bus.beginTransmission(addr);
  return bus.endTransmission() == 0;
}

// ─── Buzzer chirp (LEDC tone) ────────────────────────────────────────────────
static void chirp(uint16_t freq, uint16_t ms) {
#if P_BUZZER >= 0
  ledcAttach(P_BUZZER, freq, 10);
  ledcWriteTone(P_BUZZER, freq);
  delay(ms);
  ledcWriteTone(P_BUZZER, 0);
  ledcDetach(P_BUZZER);
#endif
}

// Status feedback (result tone, button press). On a speaker board GPIO3 is the I2S amp DIN, NOT a
// piezo — so we MUST play through I2S there, never ledcAttach() it (that would fight the I2S
// peripheral for the pin and drive the amp with a bare square wave). Piezo/mic-only boards chirp.
static void feedbackTone(uint16_t freq, uint16_t ms) {
#if USE_I2S_SPEAKER
  if (g_audioReady) audioNote((float)freq, ms, 0.9f);
#else
  chirp(freq, ms);
#endif
}

// ─── Camera config for the Hejia OV5640 pinout ───────────────────────────────
static camera_config_t makeCamConfig() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0=P_CAM_D0; c.pin_d1=P_CAM_D1; c.pin_d2=P_CAM_D2; c.pin_d3=P_CAM_D3;
  c.pin_d4=P_CAM_D4; c.pin_d5=P_CAM_D5; c.pin_d6=P_CAM_D6; c.pin_d7=P_CAM_D7;
  c.pin_xclk=P_CAM_XCLK; c.pin_pclk=P_CAM_PCLK; c.pin_vsync=P_CAM_VSYNC; c.pin_href=P_CAM_HREF;
  c.pin_sccb_sda=P_CAM_SIOD; c.pin_sccb_scl=P_CAM_SIOC;
  c.pin_pwdn=P_CAM_PWDN; c.pin_reset=P_CAM_RESET;
  c.xclk_freq_hz=20000000;
  c.pixel_format=PIXFORMAT_JPEG;
  c.frame_size=FRAMESIZE_VGA;
  c.jpeg_quality=12;
  c.fb_count=1;
  c.fb_location=CAMERA_FB_IN_PSRAM;
  c.grab_mode=CAMERA_GRAB_LATEST;
  return c;
}

static void runHardwareTests() {
  Serial.println("\n==== Hejia board hardware bring-up test ====");
  led(40, 0, 40);   // purple = running

  // 2. Buzzer / speaker
#if USE_I2S_SPEAKER
  hwtestSpeakerChime();
#else
  Serial.println("[TEST] buzzer chirp (GPIO3)…");
  chirp(2700, 120);
#endif
#if USE_I2S_MIC
  hwtestMicLevel();
#endif

  // 3. Button — expect idle HIGH (BOOT pin has an external/internal pull-up)
  pinMode(P_BUTTON, INPUT_PULLUP);
  R.button_idle_high = digitalRead(P_BUTTON) == HIGH;
  Serial.printf("[TEST] button (GPIO0) idle=%s\n", R.button_idle_high ? "HIGH ok" : "LOW (held or no pull-up)");

  // 4. PIR — just read the line (motion-dependent)
  pinMode(P_PIR, INPUT);
  R.pir_level = digitalRead(P_PIR);
  Serial.printf("[TEST] PIR (GPIO5) reads %d (wave a hand to toggle).\n", R.pir_level);

  // 5. Sensor I2C bus (38/4)
  Wire.begin(P_I2C_SDA, P_I2C_SCL, 100000);
  R.i2c_mlx = i2cPing(Wire, ADDR_MLX90640);
  R.i2c_tmp = i2cPing(Wire, ADDR_TMP112);
  R.i2c_bq  = i2cPing(Wire, ADDR_BQ27441);
  Serial.printf("[TEST] sensor I2C (SDA38/SCL4): MLX90640@0x33=%s  TMP112@0x48=%s  BQ27441@0x55=%s\n",
                R.i2c_mlx?"ok":"MISSING", R.i2c_tmp?"ok":"MISSING", R.i2c_bq?"ok":"MISSING");

  // 6a. Camera SCCB probe on its own bus (39/40) BEFORE full init
  Wire1.begin(P_CAM_SIOD, P_CAM_SIOC, 100000);
  R.cam_sccb = i2cPing(Wire1, ADDR_OV5640);
  Serial.printf("[TEST] camera SCCB (SIOD39/SIOC40): OV5640@0x3C=%s\n", R.cam_sccb?"ok":"not-acking");
  Wire1.end();

  // 6b. Full camera init + one frame
  camera_config_t cc = makeCamConfig();
  esp_err_t e = esp_camera_init(&cc);
  R.cam_err = (uint32_t)e;
  R.cam_init = (e == ESP_OK);
  if (R.cam_init) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) { R.cam_frame = true; R.frame_bytes = fb->len; esp_camera_fb_return(fb); }
    Serial.printf("[TEST] camera init OK, frame=%s (%u bytes)\n", R.cam_frame?"captured":"FAILED", (unsigned)R.frame_bytes);
    esp_camera_deinit();   // free the DVP pins/PSRAM before BLE
  } else {
    Serial.printf("[TEST] camera init FAILED err=0x%x\n", (unsigned)e);
  }

  // 7. eMMC 4-bit (14/15/16/17/18/8)
  SD_MMC.setPins(P_SD_CLK, P_SD_CMD, P_SD_D0, P_SD_D1, P_SD_D2, P_SD_D3);
  if (SD_MMC.begin("/sdcard", /*mode1bit=*/false, /*format_if_mount_failed=*/false)) {
    if (SD_MMC.cardType() != CARD_NONE) {
      R.sd_mount = true;
      R.sd_total_mb = SD_MMC.totalBytes() / (1024ULL*1024ULL);
    }
    Serial.printf("[TEST] eMMC 4-bit mount=%s total=%llu MB\n", R.sd_mount?"ok":"no-card",
                  (unsigned long long)R.sd_total_mb);
  } else {
    Serial.println("[TEST] eMMC 4-bit mount FAILED");
  }

  // ── Summary + persist ──
  bool allCore = R.i2c_mlx && R.i2c_tmp && R.i2c_bq && R.cam_init && R.cam_frame && R.sd_mount;
  Serial.printf("==== RESULT: %s ====\n", allCore ? "ALL CORE PERIPHERALS PASS" : "SOME FAILED (see above)");
  nvs.begin("hwtest", false);
  nvs.putUChar("ok", allCore ? 1 : 0);
  nvs.putUChar("mlx", R.i2c_mlx); nvs.putUChar("tmp", R.i2c_tmp); nvs.putUChar("bq", R.i2c_bq);
  nvs.putUChar("cam", R.cam_init && R.cam_frame); nvs.putUChar("sd", R.sd_mount);
  nvs.putUInt("camerr", R.cam_err); nvs.putUInt("frame", R.frame_bytes);
  nvs.end();
  led(allCore ? 0 : 60, allCore ? 60 : 0, 0);   // green / red
  feedbackTone(allCore ? 3200 : 1500, allCore ? 90 : 300);
  delay(1200);
}

// ═══════════════════ BLE OTA ═══════════════════════════════════════════════
static esp_ota_handle_t g_otaHandle = 0;
static const esp_partition_t* g_otaPart = nullptr;
static uint32_t g_otaExpected = 0, g_otaWritten = 0;
static bool g_otaActive = false;
static BLECharacteristic* g_statChar = nullptr;

static void otaNotify(const char* s) {
  Serial.printf("[OTA] %s\n", s);
  if (g_statChar) { g_statChar->setValue((uint8_t*)s, strlen(s)); g_statChar->notify(); }
}

static void otaAbort(const char* why) {
  if (g_otaActive && g_otaHandle) esp_ota_abort(g_otaHandle);
  g_otaActive = false; g_otaHandle = 0; g_otaWritten = 0; g_otaExpected = 0;
  char m[64]; snprintf(m, sizeof(m), "ABORT:%s", why); otaNotify(m);
  led(60, 0, 0);
}

class CtrlCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = c->getValue();
    if (v.length() < 1) return;
    uint8_t op = (uint8_t)v[0];
    if (op == OTA_OP_BEGIN) {
      if (v.length() < 5) { otaNotify("ERR:begin-needs-size"); return; }
      g_otaExpected = (uint32_t)(uint8_t)v[1] | ((uint32_t)(uint8_t)v[2] << 8) |
                      ((uint32_t)(uint8_t)v[3] << 16) | ((uint32_t)(uint8_t)v[4] << 24);
      g_otaPart = esp_ota_get_next_update_partition(NULL);
      if (!g_otaPart) { otaNotify("ERR:no-ota-partition"); return; }
      if (g_otaExpected == 0 || g_otaExpected > g_otaPart->size) { otaNotify("ERR:bad-size"); return; }
      esp_err_t e = esp_ota_begin(g_otaPart, g_otaExpected, &g_otaHandle);
      if (e != ESP_OK) { char m[48]; snprintf(m,sizeof(m),"ERR:begin-0x%x",(unsigned)e); otaNotify(m); return; }
      g_otaWritten = 0; g_otaActive = true;
      char m[48]; snprintf(m, sizeof(m), "BEGIN:%u", (unsigned)g_otaExpected); otaNotify(m);
      led(40, 40, 0);
    } else if (op == OTA_OP_END) {
      if (!g_otaActive) { otaNotify("ERR:no-session"); return; }
      if (g_otaWritten != g_otaExpected) { otaAbort("incomplete"); return; }
      esp_err_t e = esp_ota_end(g_otaHandle);
      if (e != ESP_OK) { char m[48]; snprintf(m,sizeof(m),"ERR:end-0x%x",(unsigned)e); otaNotify(m); g_otaActive=false; g_otaHandle=0; return; }
      e = esp_ota_set_boot_partition(g_otaPart);
      if (e != ESP_OK) { char m[48]; snprintf(m,sizeof(m),"ERR:setboot-0x%x",(unsigned)e); otaNotify(m); g_otaActive=false; return; }
      otaNotify("DONE:rebooting");
      led(0, 0, 60);
      delay(400);
      esp_restart();
    } else if (op == OTA_OP_ABORT) {
      otaAbort("host-requested");
    }
  }
};

class DataCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (!g_otaActive) return;
    String v = c->getValue();
    size_t n = v.length();
    if (n == 0) return;
    if (g_otaWritten + n > g_otaExpected) { otaAbort("overflow"); return; }
    esp_err_t e = esp_ota_write(g_otaHandle, (const uint8_t*)v.c_str(), n);
    if (e != ESP_OK) { char m[48]; snprintf(m,sizeof(m),"ERR:write-0x%x",(unsigned)e); otaNotify(m); otaAbort("write"); return; }
    g_otaWritten += n;
    // progress every ~32 KB
    static uint32_t lastPct = 255;
    uint32_t pct = (uint32_t)((uint64_t)g_otaWritten * 100 / g_otaExpected);
    if (pct != lastPct && (pct % 10 == 0)) {
      lastPct = pct; char m[32]; snprintf(m, sizeof(m), "PROG:%u", (unsigned)pct); otaNotify(m);
    }
  }
};

static void startBleOta() {
  BLEDevice::init("Stovyn-HWTEST");
  BLEDevice::setMTU(517);                       // large MTU → faster OTA
  BLEServer* srv = BLEDevice::createServer();
  BLEService* svc = srv->createService(OTA_SVC_UUID);

  BLECharacteristic* ctrl = svc->createCharacteristic(
      OTA_CTRL_UUID, BLECharacteristic::PROPERTY_WRITE);
  ctrl->setCallbacks(new CtrlCB());

  BLECharacteristic* data = svc->createCharacteristic(
      OTA_DATA_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  data->setCallbacks(new DataCB());

  g_statChar = svc->createCharacteristic(
      OTA_STAT_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  g_statChar->addDescriptor(new BLE2902());

  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(OTA_SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[OTA] BLE advertising as 'Stovyn-HWTEST' — ready for wireless update.");
  // mark this image valid so the IDF rollback watchdog doesn't revert it
  esp_ota_mark_app_valid_cancel_rollback();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[boot] Stovyn Hejia HW-TEST + BLE OTA");
  led(40, 0, 40);
  runHardwareTests();
  startBleOta();
}

void loop() {
  // Idle: slow blue pulse = advertising/ready; brief cyan while an OTA is active.
  static uint32_t t = 0; static bool up = false; static uint8_t lvl = 0;
  if (millis() - t > 40) {
    t = millis();
    lvl = up ? lvl + 3 : lvl - 3;
    if (lvl >= 45) up = false; else if (lvl <= 3) up = true;
    if (g_otaActive) led(0, lvl, lvl); else led(0, 0, lvl);
  }
  // Button chirps (proves the button end-to-end while the tech is at the bench)
  static bool wasDown = false;
  bool down = digitalRead(P_BUTTON) == LOW;
  if (down && !wasDown) feedbackTone(2500, 60);
  wasDown = down;
  delay(5);
}
