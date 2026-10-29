/*
 * buddy_p4.ino — Buddy v3.0 (Waveshare ESP32-P4)
 * ─────────────────────────────────────────────────────────────────────────────
 * Target board : Waveshare ESP32-P4-Nano / ESP32-P4-WIFI6 (also the Espressif
 *                ESP32-P4-Function-EV-Board — same audio + I2C pins)
 *   Camera  OV5647 (or OV5640) on the MIPI-CSI ribbon connector, or a USB webcam
 *   Speaker onboard ES8311 codec + amp (speaker on the SPK header)
 *   WiFi    onboard ESP32-C6 (handled by the core — plain WiFi.begin())
 *
 * Board package : esp32 by Espressif (>= 3.3.0)
 *   Tools → Board            → ESP32P4 Dev Module
 *   Tools → PSRAM            → Enabled
 *   Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)
 *   Tools → USB CDC On Boot  → Disabled  (plug into the "USB to UART" port;
 *                              Enabled only if you use the other OTG port)
 *   Tools → Chip Variant     → match your chip (boards from before late 2025
 *                              are often "before v3.00")
 *
 * Required library (Tools → Manage Libraries):
 *   WebSockets  by Markus Sattler  (>= 2.6.0)
 *
 * External wiring:
 *   3 continuous-rotation wheel servos (pots decoupled), two in front, one
 *   sideways in back — signal wires on the header:
 *     front-left → GPIO20   front-right → GPIO21   back → GPIO22
 *   Servos need their own 5V supply; tie its GND to the board's GND.
 *
 * Protocol (same as the Pi / ESP32-CAM — server.js and the web UI unchanged):
 *   Connects wss://host/ws?id=BDY-XXXXX&role=device
 *   Sends    binary 0x01 + JPEG          — video (only while a browser is paired)
 *   Sends    binary 0x02 + PCM16 mono 16 kHz — onboard mic (muted while the speaker plays)
 *   Sends    text   { "type": "hello", "audio": "pcm16" } on pairing, so the
 *                   browser sends raw PCM instead of opus (the P4 can't decode it)
 *   Receives binary 0x02 + PCM16 mono 16 kHz — hold-to-talk audio
 *   Receives text JSON:
 *     { "type": "client_connected" / "client_disconnected" }
 *     { "type": "cmd",   "dir": "fwd"|"back"|"left"|"right"|"stop" }
 *     { "type": "vec",   "vx": -1..1, "vy": -1..1 }
 *     { "type": "servo", "ch": 0-2, "angle": 0-180 }   (angle < 0 = off)
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "cJSON.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_timer.h"
#include "freertos/stream_buffer.h"
#include "esp_intr_alloc.h"
#include "esp_aec.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

// ─── Config block ─────────────────────────────────────────────────────────────
// Same layout + magic as the ESP32-CAM firmware. DO NOT reorder fields.

struct __attribute__((packed)) BuddyConfig {
  uint8_t magic[8] = { 0xBD, 0xBD, 0xBD, 0xBD, 0x42, 0x55, 0x44, 0x59 };
  char ssid[32] = "gavin iphone";
  char pass[64] = "12441122";
  char host[40] = "buddy-production-948c.up.railway.app";
  uint32_t port = 443;
  char id[16] = "BDY-00001";
} cfg;

// ─── Pins (Waveshare ESP32-P4-Nano / WIFI6) ──────────────────────────────────
#define I2C_SDA        7    // shared: ES8311 + camera SCCB
#define I2C_SCL        8
#define I2S_MCLK      13
#define I2S_BCLK      12
#define I2S_WS        10
#define I2S_DOUT       9    // → ES8311 DSDIN
#define I2S_DIN       11    // ← ES8311 ASDOUT (onboard mic)
#define SPK_PA_EN     53    // speaker amp enable

#define SERVO_PIN_FL  20
#define SERVO_PIN_FR  21
#define SERVO_PIN_B   22

// ─── Camera settings ──────────────────────────────────────────────────────────
// Low on purpose, same as the Pi: small frames = low latency. The ribbon
// camera is auto-detected (OV5647 or OV5640). The OV5647 captures a square
// 800×800 (the OV5640 1280×720); the P4's ISP + PPA turn that into RGB565 and
// scale it down, and the hardware JPEG encoder compresses it.
// 480×480 box → the square OV5647 streams 448×448 (was 288×288 at q50).
// Bigger/sharper = more data per frame: if video lags on a weak connection
// (phone hotspot), drop CAM_FPS to ~10 or go back to 320 / q50.
#define CAM_MAX_W      480
#define CAM_MAX_H      480
#define CAM_FPS         15
#define CAM_QUALITY     70     // JPEG 1-100
#define CAM_MIRROR   false
#define CAM_FLIP     false
#define CAM_SWAP     false     // set true if colors look wrong (red/blue noise)

// Camera source: true = USB webcam on the USB OTG port, false = ribbon camera
// (OV5647 / OV5640) on MIPI-CSI. CAM_MAX_W/H and CAM_FPS apply to both.
#define USE_USB_WEBCAM false

// ─── Speaker settings ─────────────────────────────────────────────────────────
#define SPK_RATE     16000
#define SPK_VOLUME     106     // 0-132, 1 dB per step: 100 = 0 dB, 106 = +6 dB.
                               // The browser already levels the voice (limiter),
                               // so big boosts here just clip — keep it ≤ ~110.

// ─── Microphone settings ──────────────────────────────────────────────────────
// Onboard mic → ES8311 → browser, as PCM16 mono 16 kHz (same as the Pi).
#define MIC_ENABLED   true
#define MIC_GAIN         5     // 0-7, each step = +6 dB (5 = 30 dB). Analog, in the codec.
                               // Kept moderate so the speaker's sound doesn't clip the
                               // mic — the echo canceller can't undo clipping.
#define MIC_BOOST      6.0f    // extra digital gain after echo cancelling (clipped).
#define MIC_CHUNK_MS   100     // audio per packet: bigger = fewer packets = fewer drops

// Full duplex: talk and listen at the same time. The ES8311 loops what it
// plays back into the mic stream (right slot), and Espressif's echo canceller
// (esp-sr AEC) subtracts it from the mic (left slot), so the browser doesn't
// hear its own voice come back. false = half duplex: the mic is muted while
// the speaker plays (and MIC_ECHO_GUARD_MS after) — the old behavior.
#define MIC_FULL_DUPLEX  true
#define AEC_SWAP_SLOTS   false // set true if the mic sounds like the speaker only
#define MIC_ECHO_GUARD_MS 300  // half-duplex mode only

// ─── Frame type bytes ─────────────────────────────────────────────────────────
#define TYPE_VIDEO  0x01
#define TYPE_AUDIO  0x02

// ─── Globals ──────────────────────────────────────────────────────────────────
WebSocketsClient ws;
volatile bool wsLive     = false;
volatile bool peerOnline = false;
i2c_master_bus_handle_t i2cBus;

struct Frame { uint8_t* buf; size_t len; };
QueueHandle_t txQueue;
#define TX_QUEUE_DEPTH 2   // keep small to minimise frame lag
QueueHandle_t micQueue;    // mic chunks waiting for loop() to send
#define MIC_QUEUE_DEPTH 10 // ~1 s of audio — rides out a slow video send

// ═════════════════════════════════════════════════════════════════════════════
// Wheels — ported from pi/buddy_pi.py
// ═════════════════════════════════════════════════════════════════════════════
// Pot-decoupled continuous-rotation servos: at idle they get NO signal at all
// (duty 0, pin low). Pulses only flow while a drive command is held. Pulses
// come from LEDC hardware PWM — jitter-free, unlike the Pi's software PWM, so
// re-check NEUTRAL_US if a wheel creeps (see README).

#define NUM_SERVOS   3
#define SERVO_FREQ   50
#define SERVO_BITS   14
#define PERIOD_US    (1000000 / SERVO_FREQ)

static const int SERVO_PINS[NUM_SERVOS] = { SERVO_PIN_FL, SERVO_PIN_FR, SERVO_PIN_B };

// Pulse matching each servo's frozen pot. Measured on the Pi.
// [front-left (ch0), front-right (ch1), back (ch2)]
static float NEUTRAL_US[NUM_SERVOS] = { 1489, 1494, 1494 };

#define DRIVE_US  150.0f   // offset from neutral at 100% commanded speed.
                           // 70 (the Pi's value) was too weak to move one of
                           // the front servos here; lower it if too fast.
#define DEADBAND   0.05f   // |speed| below this cuts the channel entirely
#define STEER_MIX  0.5f    // sideways component scale while also driving fwd/back

// Stall protection: the servo firmware cuts power after ~5 s of a position
// error that never shrinks. Rest at neutral for REST_OFF_MS every REST_ON_MS.
#define REST_ON_MS   4000
#define REST_OFF_MS  50

// Wheel drive direction in the (forward, right) frame.
static const float WHEEL_FWD[NUM_SERVOS]   = { 0.8660254f, -0.8660254f,  0.0f };
static const float WHEEL_RIGHT[NUM_SERVOS] = { 0.5f,        0.5f,       -1.0f };
static const float WHEEL_DIR[NUM_SERVOS]   = { -1.0f, -1.0f, 1.0f };  // both fronts mounted reversed
static const float WHEEL_GAIN[NUM_SERVOS]  = { 1.0f, 1.0f, 1.0f };

// FL drags → nose pulls left on fwd/back; the back wheel counter-steers.
// Flip the sign if it makes the pull worse. 0 disables.
#define BACK_ASSIST 0.15f

// Two-wheel mode (back wheel broken): the back servo gets no signal, and the
// two front wheels drive like a tank. W/S = forward/back, A/D = turn in place
// (no more sliding sideways). W+A / W+D = curve while driving.
// Set false to go back to three-wheel holonomic driving.
#define TWO_WHEEL_MODE true
#define TURN_GAIN      0.7f   // spin speed for A/D (1.0 = full speed)

// Which servo channel drives each side: 0 = GPIO20, 1 = GPIO21, 2 = GPIO22.
// Any working servo can be either wheel — e.g. if the GPIO21 servo is dead,
// mount the GPIO22 servo as the right wheel and set RIGHT_WHEEL_CH to 2.
#define LEFT_WHEEL_CH    0
#define RIGHT_WHEEL_CH   1
// Flip a sign (1.0 ↔ -1.0) if that wheel rolls backward when you press W.
#define LEFT_WHEEL_SIGN  -1.0f
#define RIGHT_WHEEL_SIGN  1.0f

static float servoTarget[NUM_SERVOS];   // commanded speed per channel, 0 = off
static SemaphoreHandle_t servoLock;

static void servoWriteUs(int ch, float us) {
  ledcWrite(SERVO_PINS[ch], (uint32_t)(us * (1 << SERVO_BITS) / PERIOD_US));
}

static void servoOff(int ch) {
  ledcWrite(SERVO_PINS[ch], 0);
}

static void setSpeed(int ch, float speed) {
  speed = constrain(speed, -1.0f, 1.0f);
  if (fabsf(speed) < DEADBAND) speed = 0.0f;
  servoTarget[ch] = speed;
  if (speed == 0.0f) servoOff(ch);
  else servoWriteUs(ch, NEUTRAL_US[ch] + speed * DRIVE_US);
}

static void drive(float vx, float vy) {
  xSemaphoreTake(servoLock, portMAX_DELAY);
  if (TWO_WHEEL_MODE) {
    // Tank steering: each side's travel speed, + = rolling forward.
    // Turning right = left side forward, right side back (and vice versa).
    float left  = vx + vy * TURN_GAIN;
    float right = vx - vy * TURN_GAIN;
    float peak  = max(fabsf(left), fabsf(right));
    if (peak > 1.0f) { left /= peak; right /= peak; }
    // 0.87 keeps W/S at the same speed as three-wheel mode.
    for (int i = 0; i < NUM_SERVOS; i++) {
      if      (i == LEFT_WHEEL_CH)  setSpeed(i, WHEEL_GAIN[i] * LEFT_WHEEL_SIGN  * 0.87f * left);
      else if (i == RIGHT_WHEEL_CH) setSpeed(i, WHEEL_GAIN[i] * RIGHT_WHEEL_SIGN * 0.87f * right);
      else                          setSpeed(i, 0.0f);  // unused servo: no signal
    }
  } else {
    for (int i = 0; i < NUM_SERVOS; i++) {
      float speed = WHEEL_DIR[i] * (WHEEL_FWD[i] * vx + WHEEL_RIGHT[i] * vy);
      if (i == 2) speed += WHEEL_DIR[2] * BACK_ASSIST * vx;  // counter the FL drag
      setSpeed(i, WHEEL_GAIN[i] * speed);
    }
  }
  xSemaphoreGive(servoLock);
  Serial.printf("[srv] drive vx=%.2f vy=%.2f → FL %+.2f  FR %+.2f  B %+.2f\n",
                vx, vy, servoTarget[0], servoTarget[1], servoTarget[2]);
}

static void driveCmd(const char* dir) {
  if      (strcmp(dir, "fwd") == 0)   drive( 1,  0);
  else if (strcmp(dir, "back") == 0)  drive(-1,  0);
  else if (strcmp(dir, "left") == 0)  drive( 0, -1);
  else if (strcmp(dir, "right") == 0) drive( 0,  1);
  else                                drive( 0,  0);  // stop / unknown
}

// Combined-key driving (W+A etc). Normalize diagonals so no wheel clips.
static void driveVec(float vx, float vy) {
  vx = constrain(vx, -1.0f, 1.0f);
  vy = constrain(vy, -1.0f, 1.0f);
  if (vx != 0.0f && vy != 0.0f) vy *= STEER_MIX;  // soften steering while driving
  float mag = sqrtf(vx * vx + vy * vy);
  if (mag > 1.0f) { vx /= mag; vy /= mag; }
  drive(vx, vy);
}

// { "type": "servo", "ch": 0-2, "angle": 0-180 } — 0-180 maps onto
// 500-2500µs (90 = 1500µs). The µs is printed: when a wheel stops, put that
// value into NEUTRAL_US. angle < 0 cuts the channel.
static void servoCalibrate(int ch, float angle) {
  if (ch < 0 || ch >= NUM_SERVOS) return;
  xSemaphoreTake(servoLock, portMAX_DELAY);
  if (angle < 0) {
    servoOff(ch);
    Serial.printf("[srv] ch%d → off\n", ch);
  } else {
    float us = 500.0f + min(180.0f, angle) * 2000.0f / 180.0f;
    servoWriteUs(ch, us);
    Serial.printf("[srv] ch%d calibrate → %.0fµs (angle %.1f)\n", ch, us, angle);
  }
  xSemaphoreGive(servoLock);
}

static void driveStop() {
  xSemaphoreTake(servoLock, portMAX_DELAY);
  for (int i = 0; i < NUM_SERVOS; i++) {
    servoTarget[i] = 0.0f;
    servoOff(i);
  }
  xSemaphoreGive(servoLock);
}

static bool anyServoActive() {
  for (int i = 0; i < NUM_SERVOS; i++)
    if (servoTarget[i] != 0.0f) return true;
  return false;
}

// Rest cycling: every REST_ON_MS of continuous driving, hold neutral for
// REST_OFF_MS ("target reached" resets the stall detector), then resume.
void restTask(void*) {
  const int stepMs = 100;
  int drivenMs = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(stepMs));
    if (!anyServoActive()) { drivenMs = 0; continue; }
    drivenMs += stepMs;
    if (drivenMs < REST_ON_MS) continue;

    xSemaphoreTake(servoLock, portMAX_DELAY);
    for (int i = 0; i < NUM_SERVOS; i++)
      if (servoTarget[i] != 0.0f) servoWriteUs(i, NEUTRAL_US[i]);
    xSemaphoreGive(servoLock);

    vTaskDelay(pdMS_TO_TICKS(REST_OFF_MS));
    drivenMs = 0;

    xSemaphoreTake(servoLock, portMAX_DELAY);
    for (int i = 0; i < NUM_SERVOS; i++)
      if (servoTarget[i] != 0.0f) servoWriteUs(i, NEUTRAL_US[i] + servoTarget[i] * DRIVE_US);
    xSemaphoreGive(servoLock);
  }
}

void initServos() {
  servoLock = xSemaphoreCreateMutex();
  for (int i = 0; i < NUM_SERVOS; i++) {
    if (!ledcAttach(SERVO_PINS[i], SERVO_FREQ, SERVO_BITS))
      Serial.printf("[srv] ch%d: PWM attach FAILED on GPIO%d\n", i, SERVO_PINS[i]);
    servoOff(i);  // no pulses until driven
  }
  xTaskCreate(restTask, "rest", 3072, NULL, 4, NULL);
  Serial.printf("[srv] LEDC servos ready (GPIO %d/%d/%d) — outputs off until driven\n",
                SERVO_PINS[0], SERVO_PINS[1], SERVO_PINS[2]);
}

// ═════════════════════════════════════════════════════════════════════════════
// Speaker — ES8311 codec, PCM16 mono 16 kHz from the browser
// ═════════════════════════════════════════════════════════════════════════════
// Audio arrives on the websocket (loop task) into a stream buffer; a separate
// task plays it, so a slow codec can never stall the websocket.

#define ES8311_ADDR   0x18
#define SPK_BUF_BYTES (SPK_RATE * 2)        // 1 s max lag before dropping
#define SPK_PREBUF    (SPK_RATE * 2 / 10)   // 100 ms cushion before playing

static i2c_master_dev_handle_t es8311;
static i2s_chan_handle_t i2sTx;
static i2s_chan_handle_t i2sRx;
static volatile int64_t spkLastPlayUs = 0;   // for the mic's echo guard
static StreamBufferHandle_t spkBuf;
static bool spkReady = false;

static bool esWrite(uint8_t reg, uint8_t val) {
  uint8_t b[2] = { reg, val };
  return i2c_master_transmit(es8311, b, 2, 100) == ESP_OK;
}

// Register sequence from Espressif's esp_codec_dev ES8311 driver, set up as a
// DAC-only I2S slave: 16 kHz, 16-bit, MCLK = 256 × fs = 4.096 MHz.
static bool initES8311() {
  bool ok = esWrite(0x00, 0x1F);            // reset
  delay(20);
  ok &= esWrite(0x00, 0x00);
  ok &= esWrite(0x01, 0x30);
  ok &= esWrite(0x02, 0x00);
  ok &= esWrite(0x03, 0x10);
  ok &= esWrite(0x16, 0x20 | constrain(MIC_GAIN, 0, 7));  // mic PGA gain
  ok &= esWrite(0x04, 0x10);
  ok &= esWrite(0x05, 0x00);
  ok &= esWrite(0x0B, 0x00);
  ok &= esWrite(0x0C, 0x00);
  ok &= esWrite(0x10, 0x1F);
  ok &= esWrite(0x11, 0x7F);
  ok &= esWrite(0x00, 0x80);                // power on, slave mode
  ok &= esWrite(0x01, 0x3F);                // MCLK from pin, all clocks on
  ok &= esWrite(0x13, 0x10);
  ok &= esWrite(0x1B, 0x0A);
  ok &= esWrite(0x1C, 0x6A);
  // Clock dividers for 4.096 MHz → 16 kHz
  ok &= esWrite(0x02, 0x00);                // pre-div 1, pre-mult 1
  ok &= esWrite(0x05, 0x00);                // adc/dac div 1
  ok &= esWrite(0x03, 0x10);                // single speed, adc osr
  ok &= esWrite(0x04, 0x10);                // dac osr
  ok &= esWrite(0x07, 0x00);                // lrck div = 0x00FF
  ok &= esWrite(0x08, 0xFF);
  ok &= esWrite(0x06, 0x03);                // bclk div 4
  // 16-bit I2S, serial data in unmuted
  ok &= esWrite(0x09, 0x0C);
  ok &= esWrite(0x0A, 0x0C);
  // Power up the DAC path
  ok &= esWrite(0x17, 0xBF);
  ok &= esWrite(0x0E, 0x02);
  ok &= esWrite(0x12, 0x00);
  ok &= esWrite(0x14, 0x1A);
  ok &= esWrite(0x0D, 0x01);
  ok &= esWrite(0x15, 0x40);
  ok &= esWrite(0x37, 0x08);
  ok &= esWrite(0x45, 0x00);
  // Echo-cancel reference: put the DAC's output on the right slot of the
  // ADC stream (left = mic, right = what's being played) — same setting
  // Espressif's esp_codec_dev uses for ES8311 AEC.
  if (MIC_FULL_DUPLEX) ok &= esWrite(0x44, 0x58);
  // Volume: DAC register, 0.5 dB steps, 0xBF = 0 dB, 0xFF = +32 dB max.
  int vol = 0xBF + (constrain(SPK_VOLUME, 0, 132) - 100) * 2;
  ok &= esWrite(0x32, (uint8_t)constrain(vol, 0, 0xFF));
  ok &= esWrite(0x31, 0x00);
  return ok;
}

// Called from the websocket handler. Never blocks — drops audio when behind.
// Whole-message drops only, so the 16-bit sample alignment can't slip.
static void speakerFeed(const uint8_t* data, size_t len) {
  if (!spkReady) return;
  len &= ~(size_t)1;
  if (len == 0 || xStreamBufferSpacesAvailable(spkBuf) < len) return;
  xStreamBufferSend(spkBuf, data, len, 0);
}

void speakerTask(void*) {
  static uint8_t chunk[512];
  bool playing = false;
  int idleMs = 0;
  for (;;) {
    if (!playing) {
      size_t avail = xStreamBufferBytesAvailable(spkBuf);
      // Wait for a cushion, unless a short clip has been sitting a while.
      if (avail < SPK_PREBUF && !(avail && idleMs >= 200)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        idleMs += 10;
        continue;
      }
      playing = true;
      idleMs = 0;
    }
    size_t n = xStreamBufferReceive(spkBuf, chunk, sizeof(chunk), pdMS_TO_TICKS(60));
    if (n == 0) { playing = false; continue; }  // underrun — rebuild the cushion
    size_t written;
    i2s_channel_write(i2sTx, chunk, n, &written, portMAX_DELAY);  // paced by the DAC
    spkLastPlayUs = esp_timer_get_time();
  }
}

static void speakerBeep();
void micTask(void*);

void initSpeaker() {
  i2c_device_config_t dev = {};
  dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev.device_address  = ES8311_ADDR;
  dev.scl_speed_hz    = 100000;
  if (i2c_master_bus_add_device(i2cBus, &dev, &es8311) != ESP_OK) {
    Serial.println("[spk] init FAILED — I2C device add"); return;
  }

  // I2S first: the ES8311 needs MCLK running before it's configured.
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.auto_clear = true;  // silence, not a looping buzz, on underrun
  // Full duplex: TX → speaker, RX ← mic, sharing the same clocks.
  if (i2s_new_channel(&chanCfg, &i2sTx, MIC_ENABLED ? &i2sRx : NULL) != ESP_OK) {
    Serial.println("[spk] init FAILED — I2S channel"); return;
  }
  i2s_std_config_t std = {};
  std.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE);
  std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;  // same audio on both slots, whichever the DAC uses
  std.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK;
  std.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
  std.gpio_cfg.ws   = (gpio_num_t)I2S_WS;
  std.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
  std.gpio_cfg.din  = MIC_ENABLED ? (gpio_num_t)I2S_DIN : I2S_GPIO_UNUSED;
  if (i2s_channel_init_std_mode(i2sTx, &std) != ESP_OK || i2s_channel_enable(i2sTx) != ESP_OK) {
    Serial.println("[spk] init FAILED — I2S config"); return;
  }
  // Mic input is read as explicit STEREO (L,R interleaved) and reduced to mono
  // in micTask — "mono" RX mode can hand back both slots, which played as
  // mono sounds half-speed and smeared.
  i2s_std_config_t stdRx = std;
  stdRx.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (MIC_ENABLED &&
      (i2s_channel_init_std_mode(i2sRx, &stdRx) != ESP_OK || i2s_channel_enable(i2sRx) != ESP_OK)) {
    Serial.println("[mic] I2S input init FAILED — mic disabled");
    i2sRx = NULL;
  }

  if (!initES8311()) {
    Serial.println("[spk] init FAILED — ES8311 not answering on I2C"); return;
  }
  pinMode(SPK_PA_EN, OUTPUT);
  digitalWrite(SPK_PA_EN, HIGH);  // amp on

  spkBuf = xStreamBufferCreate(SPK_BUF_BYTES, 2);
  xTaskCreate(speakerTask, "spk", 4096, NULL, 6, NULL);
  spkReady = true;
  Serial.printf("[spk] ES8311 ready — PCM16 mono %d Hz\n", SPK_RATE);
  speakerBeep();

  if (MIC_ENABLED && i2sRx) {
    xTaskCreatePinnedToCore(micTask, "mic", 12288, NULL, 6, NULL, 0);  // AEC needs stack
    Serial.printf("[mic] ready — gain %d (+%d dB), streams while a browser is paired\n",
                  MIC_GAIN, MIC_GAIN * 6);
  }
}

// Mic → browser: MIC_CHUNK_MS chunks of PCM16 mono 16 kHz as 0x02 frames, which
// client.js already plays (playPCM16). Handed to loop() via micQueue because
// the WebSockets library isn't thread-safe.
static int16_t* micAlloc(size_t samples) {
  // esp-sr wants 16-byte-aligned buffers
  return (int16_t*)heap_caps_aligned_alloc(16, samples * sizeof(int16_t),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void micTask(void*) {
  // Echo canceller for full duplex. If it can't start, fall back to the
  // half-duplex echo guard.
  aec_handle_t* aec = NULL;
  if (MIC_FULL_DUPLEX) {
    aec = aec_create(SPK_RATE, 4, 1, AEC_MODE_FD_HIGH_PERF);
    if (!aec) aec = aec_create(SPK_RATE, 4, 1, AEC_MODE_VOIP_HIGH_PERF);
    if (aec) Serial.printf("[mic] echo canceller on — full duplex (%d-sample frames)\n",
                           aec_get_chunksize(aec));
    else     Serial.println("[mic] echo canceller FAILED to start — using half duplex");
  }

  const size_t FRAME = aec ? aec_get_chunksize(aec) : 256;      // samples per step
  const size_t PKT   = SPK_RATE * MIC_CHUNK_MS / 1000;           // samples per packet
  int16_t* stereo = micAlloc(FRAME * 2);   // L,R pairs from the codec
  int16_t* mic    = micAlloc(FRAME);
  int16_t* ref    = micAlloc(FRAME);
  int16_t* clean  = micAlloc(FRAME);
  int16_t* pkt    = micAlloc(PKT);
  if (!stereo || !mic || !ref || !clean || !pkt) {
    Serial.println("[mic] out of memory — mic disabled");
    vTaskDelete(NULL);
  }
  // Which slot is the mic: left normally. In full-duplex mode the right slot
  // carries the speaker reference.
  const int micSide = AEC_SWAP_SLOTS ? 1 : 0;

  size_t pktFill = 0;
  int peak = 0, sent = 0, muted = 0, dropped = 0;
  int64_t lastReport = esp_timer_get_time();
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(i2sRx, stereo, FRAME * 4, &got, portMAX_DELAY) != ESP_OK ||
        got < FRAME * 4) continue;

    if (aec) {
      for (size_t i = 0; i < FRAME; i++) {
        mic[i] = stereo[2 * i + micSide];
        ref[i] = stereo[2 * i + 1 - micSide];
      }
      aec_process(aec, mic, ref, clean);   // mic minus what the speaker played
    } else {
      // Half duplex. Without the loopback, keep whichever slot is louder
      // (the codec's ADC fills one side; the other is silence or a copy).
      int side = micSide;
      if (!MIC_FULL_DUPLEX) {
        int64_t eL = 0, eR = 0;
        for (size_t i = 0; i < FRAME; i++) {
          eL += abs((int)stereo[2 * i]);
          eR += abs((int)stereo[2 * i + 1]);
        }
        side = (eR > eL) ? 1 : 0;
      }
      for (size_t i = 0; i < FRAME; i++) clean[i] = stereo[2 * i + side];
    }

    // Digital boost (clipped) into the outgoing packet.
    for (size_t i = 0; i < FRAME; i++) {
      int v = constrain((int)(clean[i] * MIC_BOOST), -32767, 32767);
      peak = max(peak, abs(v));
      pkt[pktFill++] = (int16_t)v;
      if (pktFill < PKT) continue;
      pktFill = 0;

      // A full packet: send it (or say why not).
      if (!peerOnline || !wsLive) continue;
      if (!aec && esp_timer_get_time() - spkLastPlayUs < MIC_ECHO_GUARD_MS * 1000LL) {
        muted++;   // half-duplex echo guard
        continue;
      }
      uint8_t* out = (uint8_t*)malloc(1 + PKT * 2);
      if (!out) continue;
      out[0] = TYPE_AUDIO;
      memcpy(out + 1, pkt, PKT * 2);
      Frame f = { out, 1 + PKT * 2 };
      if (xQueueSend(micQueue, &f, 0) == pdTRUE) sent++;
      else { free(out); dropped++; }   // network behind
    }

    // Once a second: how loud the mic is and what happened to the audio.
    // Peak near 0 → no sound reaching the chip; near 32767 all the time → too
    // much gain (distortion). Speech should land roughly 3000–20000.
    int64_t now = esp_timer_get_time();
    if (now - lastReport >= 1000000) {
      Serial.printf("[mic] peak %5d / 32767   sent %2d   muted %2d   dropped %2d   %s  (%s)\n",
                    peak, sent, muted, dropped, aec ? "full duplex" : "half duplex",
                    !wsLive ? "no server" : !peerOnline ? "no browser" : "streaming");
      peak = sent = muted = dropped = 0;
      lastReport = now;
    }
  }
}

// Boot test: two short rising beeps, so the speaker can be checked without
// the browser. Goes through the same buffer + task as browser audio.
#define BEEP_ON_BOOT true

static void beepTone(float freq, int ms, float amp) {
  static int16_t chunk[256];
  const int total = SPK_RATE * ms / 1000;
  const int fade  = SPK_RATE / 200;  // 5 ms fade in/out — no clicks
  for (int i = 0; i < total; ) {
    int n = min(256, total - i);
    for (int j = 0; j < n; j++, i++) {
      float env = min(1.0f, min((float)i / fade, (float)(total - i) / fade));
      chunk[j] = (int16_t)(sinf(2.0f * PI * freq * i / SPK_RATE) * amp * env * 32767);
    }
    xStreamBufferSend(spkBuf, chunk, n * 2, pdMS_TO_TICKS(100));
  }
}

static void speakerBeep() {
  if (!BEEP_ON_BOOT || !spkReady) return;
  beepTone(660, 120, 0.4f);
  beepTone(0,    60, 0.0f);   // gap
  beepTone(990, 120, 0.4f);
  Serial.println("[spk] test beep — you should hear two rising beeps");
}

// ═════════════════════════════════════════════════════════════════════════════
// Camera — OV5640 (MIPI-CSI) → PPA downscale → hardware JPEG
// ═════════════════════════════════════════════════════════════════════════════

#define CAM_BUF_COUNT 2

static int camFd = -1;
static uint8_t* camBufs[CAM_BUF_COUNT];
static uint32_t camW, camH;
static bool camReady = false;

static ppa_client_handle_t ppa;
static ppa_srm_oper_config_t srm;
static uint8_t* scaledBuf;
static size_t scaledCap;

static jpeg_encoder_handle_t jpegEnc;
static jpeg_encode_cfg_t jpegCfg;
static uint8_t* jpegBuf;
static size_t jpegCap;

static bool openCamera() {
  // SCCB rides the I2C bus we already created (shared with the ES8311).
  static esp_video_init_csi_config_t csi = {};
  csi.sccb_config.init_sccb  = false;
  csi.sccb_config.i2c_handle = i2cBus;
  csi.sccb_config.freq       = 100000;
  csi.reset_pin = GPIO_NUM_NC;
  csi.pwdn_pin  = GPIO_NUM_NC;
  esp_video_init_config_t vcfg = {};
  vcfg.csi = &csi;
  if (esp_video_init(&vcfg) != ESP_OK) {
    Serial.println("[cam] init FAILED — is the camera ribbon seated (contacts the right way)?");
    return false;
  }

  camFd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (camFd < 0) { Serial.println("[cam] open FAILED — sensor not detected"); return false; }

  struct v4l2_capability cap = {};
  if (ioctl(camFd, VIDIOC_QUERYCAP, &cap) == 0)
    Serial.printf("[cam] sensor: %s\n", (char*)cap.card);

  // Keep the sensor's native size (1280×720 by default), ask for RGB565.
  struct v4l2_format fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(camFd, VIDIOC_G_FMT, &fmt) != 0) return false;
  if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(camFd, VIDIOC_S_FMT, &fmt) != 0) {
      Serial.println("[cam] sensor can't output RGB565"); return false;
    }
  }
  camW = fmt.fmt.pix.width;
  camH = fmt.fmt.pix.height;

  struct v4l2_requestbuffers req = {};
  req.count  = CAM_BUF_COUNT;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (ioctl(camFd, VIDIOC_REQBUFS, &req) != 0) return false;

  for (int i = 0; i < CAM_BUF_COUNT; i++) {
    struct v4l2_buffer buf = {};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = i;
    if (ioctl(camFd, VIDIOC_QUERYBUF, &buf) != 0) return false;
    camBufs[i] = (uint8_t*)mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, camFd, buf.m.offset);
    if (!camBufs[i] || camBufs[i] == MAP_FAILED) return false;
    if (ioctl(camFd, VIDIOC_QBUF, &buf) != 0) return false;
  }

  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  return ioctl(camFd, VIDIOC_STREAMON, &type) == 0;
}

// Pick a uniform PPA scale (1/16 steps) that fits CAM_MAX_W × CAM_MAX_H and
// center-crop the input so the output is a multiple of 16 on both sides —
// the JPEG encoder's 4:2:0 macroblocks need that.
static bool initScalerAndEncoder() {
  float s = min((float)CAM_MAX_W / camW, (float)CAM_MAX_H / camH);
  s = min(s, 1.0f);
  s = max(floorf(s * 16.0f) / 16.0f, 1.0f / 16.0f);

  uint32_t outW = ((uint32_t)(camW * s)) & ~15u;
  uint32_t outH = ((uint32_t)(camH * s)) & ~15u;
  uint32_t blkW = min(camW, (uint32_t)(outW / s));
  uint32_t blkH = min(camH, (uint32_t)(outH / s));

  // The scaled frame is the PPA's output AND the JPEG encoder's input. The PPA
  // needs address + size aligned to the PSRAM's L2 cache line (128 B), which
  // jpeg_alloc_encoder_mem doesn't guarantee — align to 256 B ourselves.
  const size_t ALIGN = 256;
  scaledCap = (outW * outH * 2 + ALIGN - 1) & ~(ALIGN - 1);
  scaledBuf = (uint8_t*)heap_caps_aligned_calloc(ALIGN, 1, scaledCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  jpeg_encode_memory_alloc_cfg_t outMem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
  jpegBuf   = (uint8_t*)jpeg_alloc_encoder_mem(outW * outH, &outMem, &jpegCap);
  if (!scaledBuf || !jpegBuf) { Serial.println("[cam] out of memory — is PSRAM enabled?"); return false; }

  ppa_client_config_t pcfg = {};
  pcfg.oper_type = PPA_OPERATION_SRM;
  if (ppa_register_client(&pcfg, &ppa) != ESP_OK) return false;

  srm = {};
  srm.in.pic_w          = camW;
  srm.in.pic_h          = camH;
  srm.in.block_w        = blkW;
  srm.in.block_h        = blkH;
  srm.in.block_offset_x = (camW - blkW) / 2;
  srm.in.block_offset_y = (camH - blkH) / 2;
  srm.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
  srm.out.buffer        = scaledBuf;
  srm.out.buffer_size   = scaledCap;
  srm.out.pic_w         = outW;
  srm.out.pic_h         = outH;
  srm.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
  srm.rotation_angle    = PPA_SRM_ROTATION_ANGLE_0;
  srm.scale_x           = s;
  srm.scale_y           = s;
  srm.mirror_x          = CAM_MIRROR;
  srm.mirror_y          = CAM_FLIP;
  srm.byte_swap         = CAM_SWAP;
  srm.mode              = PPA_TRANS_MODE_BLOCKING;

  jpeg_encode_engine_cfg_t ecfg = {};
  ecfg.timeout_ms = 100;
  if (jpeg_new_encoder_engine(&ecfg, &jpegEnc) != ESP_OK) return false;
  jpegCfg = {};
  jpegCfg.width         = outW;
  jpegCfg.height        = outH;
  jpegCfg.src_type      = JPEG_ENCODE_IN_FORMAT_RGB565;
  jpegCfg.sub_sample    = JPEG_DOWN_SAMPLING_YUV420;
  jpegCfg.image_quality = CAM_QUALITY;

  Serial.printf("[cam] ready — capture %lux%lu → stream %lux%lu @ %d fps, q%d\n",
                (unsigned long)camW, (unsigned long)camH,
                (unsigned long)outW, (unsigned long)outH, CAM_FPS, CAM_QUALITY);
  return true;
}

void initCamera() {
  camReady = openCamera() && initScalerAndEncoder();
  if (!camReady) Serial.println("[cam] disabled");
}

static size_t encodeFrame(const uint8_t* frame) {
  srm.in.buffer = frame;
  if (ppa_do_scale_rotate_mirror(ppa, &srm) != ESP_OK) return 0;
  uint32_t outLen = 0;
  if (jpeg_encoder_process(jpegEnc, &jpegCfg, scaledBuf, jpegCfg.width * jpegCfg.height * 2,
                           jpegBuf, jpegCap, &outLen) != ESP_OK) return 0;
  return outLen;
}

void cameraTask(void*) {
  const int64_t periodUs = 1000000 / CAM_FPS;
  int64_t nextUs = 0;
  for (;;) {
    struct v4l2_buffer buf = {};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(camFd, VIDIOC_DQBUF, &buf) != 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    size_t len = 0;
    int64_t now = esp_timer_get_time();
    if (peerOnline && now >= nextUs) {
      nextUs = now + periodUs;
      len = encodeFrame(camBufs[buf.index]);
    }
    ioctl(camFd, VIDIOC_QBUF, &buf);  // hand the buffer straight back to the sensor

    if (len) {
      // Hand off to loop() — the WebSockets library isn't thread-safe.
      uint8_t* out = (uint8_t*)ps_malloc(1 + len);
      if (!out) continue;
      out[0] = TYPE_VIDEO;
      memcpy(out + 1, jpegBuf, len);
      Frame f = { out, 1 + len };
      if (xQueueSend(txQueue, &f, 0) != pdTRUE) free(out);
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// USB webcam (UVC) — the P4 is the USB host on its OTG port
// ═════════════════════════════════════════════════════════════════════════════
// Webcams already send MJPEG, i.e. a JPEG per frame, so frames go straight to
// the browser — no scaling or re-encoding. We ask for the smallest MJPEG size
// that still covers CAM_MAX_W × CAM_MAX_H, to keep frames small and lag low.
// Plugging and unplugging is handled: the stream reopens on reconnect.

static volatile int  uvcNewAddr   = -1;  // set when a webcam is plugged in
static volatile int  uvcNewIdx    = 0;
static volatile int  uvcNewFrames = 0;
static volatile bool uvcLost      = false;
static uvc_host_stream_hdl_t uvcStream = NULL;
static int64_t uvcNextUs = 0;

static void usbHostTask(void*) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

static void uvcDriverEvent(const uvc_host_driver_event_data_t* e, void*) {
  if (e->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) return;
  uvcNewIdx    = e->device_connected.uvc_stream_index;
  uvcNewFrames = e->device_connected.frame_info_num;
  uvcNewAddr   = e->device_connected.dev_addr;
}

static void uvcStreamEvent(const uvc_host_stream_event_data_t* e, void*) {
  if (e->type == UVC_HOST_DEVICE_DISCONNECTED) uvcLost = true;
}

// Runs in the UVC driver task: copy the frame out and hand the buffer back.
static bool uvcFrame(const uvc_host_frame_t* f, void*) {
  int64_t now = esp_timer_get_time();
  if (peerOnline && f->data_len > 0 && now >= uvcNextUs) {
    uvcNextUs = now + 1000000 / CAM_FPS;
    uint8_t* out = (uint8_t*)ps_malloc(1 + f->data_len);
    if (out) {
      out[0] = TYPE_VIDEO;
      memcpy(out + 1, f->data, f->data_len);
      Frame fr = { out, 1 + f->data_len };
      if (xQueueSend(txQueue, &fr, 0) != pdTRUE) free(out);  // loop() sends it
    }
  }
  return true;  // frame buffer can be reused right away
}

// Smallest MJPEG size that covers CAM_MAX_W × CAM_MAX_H (or the biggest
// available if none does). Falls back to the camera's default format.
static uvc_host_stream_format_t pickWebcamFormat(uint8_t addr, uint8_t idx, int count) {
  uvc_host_stream_format_t fmt = { 0, 0, 0, UVC_VS_FORMAT_DEFAULT };
  size_t n = count > 0 ? count : 16;
  uvc_host_frame_info_t* list = (uvc_host_frame_info_t*)calloc(n, sizeof(uvc_host_frame_info_t));
  if (!list) return fmt;
  if (uvc_host_get_frame_list(addr, idx, (uvc_host_frame_info_t (*)[])list, &n) != ESP_OK) n = 0;

  int best = -1, biggest = -1;
  for (size_t i = 0; i < n; i++) {
    Serial.printf("[usb]   %s %ux%u\n", list[i].format == UVC_VS_FORMAT_MJPEG ? "MJPEG" : "other",
                  list[i].h_res, list[i].v_res);
    if (list[i].format != UVC_VS_FORMAT_MJPEG) continue;
    unsigned area = list[i].h_res * list[i].v_res;
    if (biggest < 0 || area > list[biggest].h_res * list[biggest].v_res) biggest = i;
    if (list[i].h_res >= CAM_MAX_W && list[i].v_res >= CAM_MAX_H &&
        (best < 0 || area < list[best].h_res * list[best].v_res)) best = i;
  }
  if (best < 0) best = biggest;
  if (best >= 0) {
    fmt.h_res  = list[best].h_res;
    fmt.v_res  = list[best].v_res;
    fmt.format = UVC_VS_FORMAT_MJPEG;
  }
  free(list);
  return fmt;
}

static void openWebcam(uint8_t addr, uint8_t idx, int frameCount) {
  Serial.printf("[usb] webcam connected (addr %u) — formats:\n", addr);
  uvc_host_stream_format_t fmt = pickWebcamFormat(addr, idx, frameCount);
  if (fmt.format != UVC_VS_FORMAT_MJPEG)
    Serial.println("[usb] no MJPEG mode found — this webcam may not work with Buddy");

  uvc_host_stream_config_t sc = {};
  sc.event_cb = uvcStreamEvent;
  sc.frame_cb = uvcFrame;
  sc.usb.dev_addr         = addr;
  sc.usb.uvc_stream_index = idx;
  sc.vs_format = fmt;
  sc.advanced.number_of_frame_buffers = 3;
  sc.advanced.frame_size      = 0;                  // size from the camera's negotiation
  sc.advanced.frame_heap_caps = MALLOC_CAP_SPIRAM;
  sc.advanced.number_of_urbs  = 3;
  sc.advanced.urb_size        = 10 * 1024;

  // Ask for CAM_FPS first; if the camera refuses, take its default rate
  // (extra frames are skipped in uvcFrame anyway).
  sc.vs_format.fps = CAM_FPS;
  esp_err_t err = uvc_host_stream_open(&sc, pdMS_TO_TICKS(5000), &uvcStream);
  if (err != ESP_OK) {
    sc.vs_format.fps = 0;
    err = uvc_host_stream_open(&sc, pdMS_TO_TICKS(5000), &uvcStream);
  }
  if (err != ESP_OK) {
    Serial.printf("[usb] webcam open FAILED (%s)\n", esp_err_to_name(err));
    uvcStream = NULL;
    return;
  }
  if (uvc_host_stream_start(uvcStream) != ESP_OK) {
    Serial.println("[usb] webcam start FAILED");
    uvc_host_stream_close(uvcStream);
    uvcStream = NULL;
    return;
  }
  camReady = true;
  Serial.printf("[usb] webcam streaming MJPEG %ux%u\n", fmt.h_res, fmt.v_res);
}

void webcamTask(void*) {
  for (;;) {
    if (uvcLost) {
      uvcLost = false;
      Serial.println("[usb] webcam unplugged");
      if (uvcStream) { uvc_host_stream_close(uvcStream); uvcStream = NULL; }
      camReady = false;
    }
    if (!uvcStream && uvcNewAddr >= 0) {
      int addr = uvcNewAddr;
      uvcNewAddr = -1;
      openWebcam(addr, uvcNewIdx, uvcNewFrames);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void initWebcam() {
  usb_host_config_t hc = {};
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;   // peripheral_map 0 = the high-speed OTG port
  if (usb_host_install(&hc) != ESP_OK) {
    Serial.println("[usb] USB host init FAILED — webcam disabled"); return;
  }
  xTaskCreatePinnedToCore(usbHostTask, "usbh", 4096, NULL, 10, NULL, 0);

  uvc_host_driver_config_t dc = {};
  dc.driver_task_stack_size = 6144;
  dc.driver_task_priority   = 9;
  dc.xCoreID                = tskNO_AFFINITY;
  dc.create_background_task = true;
  dc.event_cb               = uvcDriverEvent;
  if (uvc_host_install(&dc) != ESP_OK) {
    Serial.println("[usb] UVC driver init FAILED — webcam disabled"); return;
  }
  xTaskCreate(webcamTask, "webcam", 6144, NULL, 5, NULL);
  Serial.println("[usb] waiting for a webcam on the USB OTG port...");
}

// ═════════════════════════════════════════════════════════════════════════════
// WebSocket
// ═════════════════════════════════════════════════════════════════════════════

static double jsonNum(cJSON* doc, const char* key, double def) {
  cJSON* v = cJSON_GetObjectItem(doc, key);
  return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void handleText(const char* txt, size_t len) {
  cJSON* doc = cJSON_ParseWithLength(txt, len);
  if (!doc) return;
  cJSON* jt = cJSON_GetObjectItem(doc, "type");
  const char* t = cJSON_IsString(jt) ? jt->valuestring : "";

  if (strcmp(t, "client_connected") == 0) {
    peerOnline = true;
    Serial.println("[ws]  browser online");
    ws.sendTXT("{\"type\":\"hello\",\"audio\":\"pcm16\",\"rate\":16000}");
  } else if (strcmp(t, "client_disconnected") == 0) {
    peerOnline = false;
    driveStop();
    Serial.println("[ws]  browser offline");
  } else if (strcmp(t, "cmd") == 0) {
    cJSON* dir = cJSON_GetObjectItem(doc, "dir");
    driveCmd(cJSON_IsString(dir) ? dir->valuestring : "stop");
  } else if (strcmp(t, "vec") == 0) {
    driveVec(jsonNum(doc, "vx", 0), jsonNum(doc, "vy", 0));
  } else if (strcmp(t, "servo") == 0) {
    servoCalibrate((int)jsonNum(doc, "ch", 0), jsonNum(doc, "angle", 90));
  }
  cJSON_Delete(doc);
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      wsLive = true;
      Serial.printf("[ws]  connected — id: %s\n", cfg.id);
      break;

    case WStype_DISCONNECTED:
      wsLive = false;
      peerOnline = false;
      driveStop();
      Serial.printf("[ws]  disconnected (WiFi %s)\n",
                    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "NOT connected");
      break;

    case WStype_TEXT:
      handleText((const char*)payload, len);
      break;

    case WStype_BIN:
      if (len > 1 && payload[0] == TYPE_AUDIO) speakerFeed(payload + 1, len - 1);
      break;

    default: break;
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Setup / loop
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("\n[buddy] booting (ESP32-P4) — id: %s\n", cfg.id);
  if (!psramFound()) Serial.println("[buddy] WARNING: no PSRAM — Tools → PSRAM → Enabled");

  txQueue  = xQueueCreate(TX_QUEUE_DEPTH, sizeof(Frame));
  micQueue = xQueueCreate(MIC_QUEUE_DEPTH, sizeof(Frame));

  // One I2C bus for the ES8311 and the camera's SCCB.
  i2c_master_bus_config_t busCfg = {};
  busCfg.i2c_port          = I2C_NUM_0;
  busCfg.sda_io_num        = (gpio_num_t)I2C_SDA;
  busCfg.scl_io_num        = (gpio_num_t)I2C_SCL;
  busCfg.clk_source        = I2C_CLK_SRC_DEFAULT;
  busCfg.glitch_ignore_cnt = 7;
  busCfg.flags.enable_internal_pullup = true;
  ESP_ERROR_CHECK(i2c_new_master_bus(&busCfg, &i2cBus));

  initServos();
  initSpeaker();
  if (USE_USB_WEBCAM) initWebcam();
  else                initCamera();

  // WiFi (through the onboard ESP32-C6)
  Serial.printf("[wifi] connecting to \"%s\"\n", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // power save adds tens of ms of latency

  // Report the C6 coprocessor link. If the C6 isn't answering, or its
  // firmware is older than this core expects, WiFi sees nothing at all.
  uint32_t hMaj, hMin, hPat, sMaj, sMin, sPat;
  hostedGetHostVersion(&hMaj, &hMin, &hPat);
  hostedGetSlaveVersion(&sMaj, &sMin, &sPat);
  Serial.printf("[wifi] C6 link: %s — host fw %lu.%lu.%lu, C6 fw %lu.%lu.%lu (%s)\n",
                hostedIsWiFiActive() ? "up" : "DOWN",
                (unsigned long)hMaj, (unsigned long)hMin, (unsigned long)hPat,
                (unsigned long)sMaj, (unsigned long)sMin, (unsigned long)sPat,
                hostedGetSlaveTargetName() ? hostedGetSlaveTargetName() : "?");
  if (hostedHasUpdate())
    Serial.printf("[wifi] C6 firmware is OUTDATED — update it (see README). Image: %s\n",
                  hostedGetUpdateURL());
  WiFi.begin(cfg.ssid, cfg.pass);
  WiFi.setAutoReconnect(true);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\n[wifi] %s\n", WiFi.localIP().toString().c_str());
  else {
    // Show what the radio can actually see — catches 5 GHz-only hotspots and
    // look-alike names (iPhones use a curly ’ in "Name’s iPhone").
    Serial.printf("\n[wifi] could not join \"%s\" — networks visible (2.4 GHz only):\n", cfg.ssid);
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++)
      Serial.printf("[wifi]   \"%s\"  (%d dBm)\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    if (n == 0) Serial.println("[wifi]   (none)");
    if (n < 0)  Serial.printf("[wifi]   scan FAILED (code %d) — the C6 WiFi chip isn't responding\n", n);
    WiFi.begin(cfg.ssid, cfg.pass);  // the scan interrupts the join — start it again
  }

  // WebSocket — WSS on 443 (Railway), plain WS otherwise (local hub)
  String path = String("/ws?id=") + cfg.id + "&role=device";
  bool useSSL = (cfg.port == 443);
  Serial.printf("[ws]  → %s:%lu%s (SSL: %s)\n", cfg.host, (unsigned long)cfg.port,
                path.c_str(), useSSL ? "yes" : "no");
  if (useSSL) ws.beginSSL(cfg.host, 443, path.c_str());
  else        ws.begin(cfg.host, (uint16_t)cfg.port, path.c_str());
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);
  ws.enableHeartbeat(15000, 3000, 2);

  if (!USE_USB_WEBCAM && camReady) xTaskCreatePinnedToCore(cameraTask, "cam", 6144, NULL, 5, NULL, 1);

  Serial.println("[buddy] running");
}

void loop() {
  Frame f;
  // Mic audio first — it's small and late audio is worse than a late frame.
  while (xQueueReceive(micQueue, &f, 0) == pdTRUE) {
    if (wsLive && peerOnline) ws.sendBIN(f.buf, f.len);
    free(f.buf);
  }
  while (xQueueReceive(txQueue, &f, 0) == pdTRUE) {
    if (wsLive && peerOnline) ws.sendBIN(f.buf, f.len);
    free(f.buf);
  }

  ws.loop();

  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 5000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }
}
