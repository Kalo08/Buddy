/*
 * buddy_p4.ino — Buddy v3.0 (Waveshare ESP32-P4)
 * ─────────────────────────────────────────────────────────────────────────────
 * Target board : Waveshare ESP32-P4-Nano / ESP32-P4-WIFI6 (also the Espressif
 *                ESP32-P4-Function-EV-Board — same audio + I2C pins)
 *   Camera  OV5640 on the MIPI-CSI ribbon connector
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

// ─── Config block ─────────────────────────────────────────────────────────────
// Same layout + magic as the ESP32-CAM firmware. DO NOT reorder fields.

struct __attribute__((packed)) BuddyConfig {
  uint8_t magic[8] = { 0xBD, 0xBD, 0xBD, 0xBD, 0x42, 0x55, 0x44, 0x59 };
  char ssid[32] = "Gavin's iphone";
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
#define SPK_PA_EN     53    // speaker amp enable

#define SERVO_PIN_FL  20
#define SERVO_PIN_FR  21
#define SERVO_PIN_B   22

// ─── Camera settings ──────────────────────────────────────────────────────────
// Low on purpose, same as the Pi: small frames = low latency. The OV5640
// captures 1280×720; the P4's PPA scales it down and the hardware JPEG
// encoder compresses it — the CPU never touches a pixel.
#define CAM_MAX_W      320
#define CAM_MAX_H      240
#define CAM_FPS         15
#define CAM_QUALITY     50     // JPEG 1-100
#define CAM_MIRROR   false
#define CAM_FLIP     false
#define CAM_SWAP     false     // set true if colors look wrong (red/blue noise)

// ─── Speaker settings ─────────────────────────────────────────────────────────
#define SPK_RATE     16000
#define SPK_VOLUME      90     // 0-100 (100 = 0 dB, each step below = -1 dB)

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

#define DRIVE_US   70.0f   // offset from neutral at 100% commanded speed
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
  for (int i = 0; i < NUM_SERVOS; i++) {
    float speed = WHEEL_DIR[i] * (WHEEL_FWD[i] * vx + WHEEL_RIGHT[i] * vy);
    if (i == 2) speed += WHEEL_DIR[2] * BACK_ASSIST * vx;  // counter the FL drag
    setSpeed(i, WHEEL_GAIN[i] * speed);
  }
  xSemaphoreGive(servoLock);
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
    ledcAttach(SERVO_PINS[i], SERVO_FREQ, SERVO_BITS);
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
  ok &= esWrite(0x16, 0x24);
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
  // Volume (0xBF = 0 dB, 0.5 dB steps) + unmute
  int vol = 0xBF - (100 - constrain(SPK_VOLUME, 0, 100)) * 2;
  ok &= esWrite(0x32, (uint8_t)max(vol, 0));
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
  }
}

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
  if (i2s_new_channel(&chanCfg, &i2sTx, NULL) != ESP_OK) {
    Serial.println("[spk] init FAILED — I2S channel"); return;
  }
  i2s_std_config_t std = {};
  std.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE);
  std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  std.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK;
  std.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
  std.gpio_cfg.ws   = (gpio_num_t)I2S_WS;
  std.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
  std.gpio_cfg.din  = I2S_GPIO_UNUSED;
  if (i2s_channel_init_std_mode(i2sTx, &std) != ESP_OK || i2s_channel_enable(i2sTx) != ESP_OK) {
    Serial.println("[spk] init FAILED — I2S config"); return;
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
    Serial.println("[cam] init FAILED — is the OV5640 ribbon seated (contacts the right way)?");
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

  jpeg_encode_memory_alloc_cfg_t inMem  = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
  jpeg_encode_memory_alloc_cfg_t outMem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
  scaledBuf = (uint8_t*)jpeg_alloc_encoder_mem(outW * outH * 2, &inMem, &scaledCap);
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
      Serial.println("[ws]  disconnected");
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

  txQueue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(Frame));

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
  initCamera();

  // WiFi (through the onboard ESP32-C6)
  Serial.printf("[wifi] connecting to \"%s\"\n", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // power save adds tens of ms of latency
  WiFi.begin(cfg.ssid, cfg.pass);
  WiFi.setAutoReconnect(true);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\n[wifi] %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\n[wifi] timed out — will retry");

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

  if (camReady) xTaskCreatePinnedToCore(cameraTask, "cam", 6144, NULL, 5, NULL, 1);

  Serial.println("[buddy] running");
}

void loop() {
  Frame f;
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
