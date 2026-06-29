/*
 * buddy_firmware.ino — Buddy v1.0
 * ─────────────────────────────────────────────────────────────────────────────
 * Target board : Seeed Studio XIAO ESP32S3 Sense
 *
 * Required libraries (Tools → Manage Libraries):
 *   WebSockets  by Markus Sattler  (>= 2.4.0)
 *   ArduinoJson by Benoit Blanchon  (>= 6.x)
 *
 * Board package : esp32 by Espressif (>= 2.0.14)
 * Board select  : XIAO_ESP32S3  (or "Seeed Studio XIAO ESP32S3")
 * PSRAM         : Tools → PSRAM → OPI PSRAM   ← important, enables 8MB PSRAM
 *
 * Hardware on the Sense board (no extra wiring needed):
 *   Camera  OV2640  — connected via flex cable to expansion board
 *   Mic     PDM     — onboard, CLK=GPIO42  DATA=GPIO41
 *
 * External wiring needed:
 *   MAX98357A speaker amp → BCLK=D8(GPIO7)  LRC=D7(GPIO44)  DIN=D6(GPIO43)
 *   Motor driver (L298N)  → see MOTOR_* defines below
 *
 * Config block is patched by the Buddy flash tool before writing.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"

// ─── Config block ─────────────────────────────────────────────────────────────
// DO NOT reorder fields. Flash tool patches at fixed offsets from magic bytes.

struct __attribute__((packed)) BuddyConfig {
  uint8_t  magic[8] = {0xBD,0xBD,0xBD,0xBD,0x42,0x55,0x44,0x59};
  char     ssid[32] = "YOUR_SSID";
  char     pass[64] = "YOUR_PASS";
  char     host[40] = "192.168.1.x";
  uint32_t port     = 3000;
  char     id[16]   = "BDY-00001";
} cfg;

// ─── Camera pins (XIAO ESP32S3 Sense) ────────────────────────────────────────
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

// ─── PDM microphone (onboard) ─────────────────────────────────────────────────
#define MIC_I2S     I2S_NUM_0
#define MIC_CLK     42         // PDM clock
#define MIC_DATA    41         // PDM data
#define MIC_RATE    16000
#define MIC_MS      30         // chunk size → 30ms × 16000 = 480 samples

// ─── I2S speaker (external MAX98357A) ─────────────────────────────────────────
#define SPK_I2S     I2S_NUM_1
#define SPK_BCLK    7          // D8
#define SPK_LRC     44         // D7
#define SPK_DIN     43         // D6
#define SPK_RATE    16000

// ─── Motor driver ─────────────────────────────────────────────────────────────
// Wire to an L298N, DRV8833, or similar. Change pins to match your wiring.
#define MTR_A_FWD    1         // D0
#define MTR_A_BWD    2         // D1
#define MTR_B_FWD    3         // D2
#define MTR_B_BWD    4         // D3

// ─── Frame type bytes ─────────────────────────────────────────────────────────
#define TYPE_VIDEO  0x01
#define TYPE_AUDIO  0x02

// ─── Globals ──────────────────────────────────────────────────────────────────
WebSocketsClient ws;
volatile bool wsLive     = false;
volatile bool peerOnline = false;

struct Frame { uint8_t* buf; size_t len; };
QueueHandle_t txQueue;
#define TX_QUEUE_DEPTH 3

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool queueFrame(uint8_t* buf, size_t len) {
  Frame f = {buf, len};
  if (xQueueSend(txQueue, &f, 0) == pdTRUE) return true;
  free(buf);
  return false;
}

static void stopMotors() {
  digitalWrite(MTR_A_FWD, LOW); digitalWrite(MTR_A_BWD, LOW);
  digitalWrite(MTR_B_FWD, LOW); digitalWrite(MTR_B_BWD, LOW);
}

static void driveMotors(const char* dir) {
  stopMotors();
  if      (strcmp(dir, "fwd")   == 0) { digitalWrite(MTR_A_FWD,HIGH); digitalWrite(MTR_B_FWD,HIGH); }
  else if (strcmp(dir, "back")  == 0) { digitalWrite(MTR_A_BWD,HIGH); digitalWrite(MTR_B_BWD,HIGH); }
  else if (strcmp(dir, "left")  == 0) { digitalWrite(MTR_A_BWD,HIGH); digitalWrite(MTR_B_FWD,HIGH); }
  else if (strcmp(dir, "right") == 0) { digitalWrite(MTR_A_FWD,HIGH); digitalWrite(MTR_B_BWD,HIGH); }
  Serial.printf("[mtr] %s\n", dir);
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket event handler
// ─────────────────────────────────────────────────────────────────────────────
void onWsEvent(WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      wsLive = true;
      Serial.printf("[ws]  connected — id: %s\n", cfg.id);
      break;

    case WStype_DISCONNECTED:
      wsLive = false; peerOnline = false;
      stopMotors();
      Serial.println("[ws]  disconnected");
      break;

    case WStype_TEXT: {
      StaticJsonDocument<128> doc;
      if (deserializeJson(doc, payload, len) != DeserializationError::Ok) break;
      const char* t = doc["type"] | "";
      if      (strcmp(t, "client_connected")    == 0) { peerOnline = true;  Serial.println("[ws]  browser online");  }
      else if (strcmp(t, "client_disconnected") == 0) { peerOnline = false; Serial.println("[ws]  browser offline"); stopMotors(); }
      else if (strcmp(t, "cmd")                 == 0) { driveMotors(doc["dir"] | "stop"); }
      break;
    }

    case WStype_BIN:
      // Audio from browser mic → play on speaker
      if (len > 1 && payload[0] == TYPE_AUDIO) {
        size_t written = 0;
        i2s_write(SPK_I2S, payload + 1, len - 1, &written, pdMS_TO_TICKS(20));
      }
      break;

    default: break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Camera task (Core 0)
// ─────────────────────────────────────────────────────────────────────────────
void cameraTask(void*) {
  const TickType_t interval = pdMS_TO_TICKS(66); // ~15 fps
  TickType_t wake = xTaskGetTickCount();

  for (;;) {
    if (peerOnline) {
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb && fb->format == PIXFORMAT_JPEG) {
        uint8_t* buf = (uint8_t*)malloc(1 + fb->len);
        if (buf) {
          buf[0] = TYPE_VIDEO;
          memcpy(buf + 1, fb->buf, fb->len);
          queueFrame(buf, 1 + fb->len);
        }
      }
      if (fb) esp_camera_fb_return(fb);
    }
    vTaskDelayUntil(&wake, interval);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Mic task (Core 1)
// ─────────────────────────────────────────────────────────────────────────────
void micTask(void*) {
  // PDM on XIAO ESP32S3 reads directly as 16-bit PCM — no shift needed
  const int    nSamples = MIC_RATE * MIC_MS / 1000;
  const size_t pcmBytes = nSamples * sizeof(int16_t);

  uint8_t* buf = (uint8_t*)malloc(1 + pcmBytes);
  if (!buf) { Serial.println("[mic] malloc fail"); vTaskDelete(NULL); }
  buf[0] = TYPE_AUDIO;

  for (;;) {
    size_t got = 0;
    i2s_read(MIC_I2S, buf + 1, pcmBytes, &got, pdMS_TO_TICKS(200));
    if (got > 0 && peerOnline) {
      // Copy so the queue owns its own buffer
      uint8_t* frame = (uint8_t*)malloc(1 + got);
      if (frame) {
        frame[0] = TYPE_AUDIO;
        memcpy(frame + 1, buf + 1, got);
        queueFrame(frame, 1 + got);
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Hardware init
// ─────────────────────────────────────────────────────────────────────────────
void initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk     = XCLK_GPIO_NUM;
  c.pin_pclk     = PCLK_GPIO_NUM;
  c.pin_vsync    = VSYNC_GPIO_NUM;
  c.pin_href     = HREF_GPIO_NUM;
  c.pin_sscb_sda = SIOD_GPIO_NUM;
  c.pin_sscb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn     = PWDN_GPIO_NUM;
  c.pin_reset    = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = FRAMESIZE_VGA;   // 640×480 — try FRAMESIZE_SVGA for more detail
  c.jpeg_quality = 12;
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;  // use the 8MB PSRAM for frame buffers
  c.grab_mode    = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&c) != ESP_OK) {
    Serial.println("[cam] init FAILED — check expansion board is attached"); return;
  }
  sensor_t* s = esp_camera_sensor_get();
  s->set_whitebal(s, 1); s->set_awb_gain(s, 1);
  s->set_exposure_ctrl(s, 1); s->set_gain_ctrl(s, 1);
  s->set_raw_gma(s, 1);  s->set_lenc(s, 1);
  s->set_vflip(s, 0);    s->set_hmirror(s, 0);
  Serial.println("[cam] ready");
}

void initMic() {
  // PDM mode — onboard microphone on XIAO ESP32S3 Sense
  i2s_config_t mc = {};
  mc.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  mc.sample_rate          = MIC_RATE;
  mc.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  mc.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  mc.communication_format = I2S_COMM_FORMAT_STAND_PCM_SHORT;
  mc.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  mc.dma_buf_count        = 4;
  mc.dma_buf_len          = 256;
  mc.use_apll             = false;  // PDM doesn't use APLL

  // For PDM: ws_io_num = CLK, data_in_num = DATA
  i2s_pin_config_t mp = {
    .bck_io_num   = I2S_PIN_NO_CHANGE,
    .ws_io_num    = MIC_CLK,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_DATA
  };

  if (i2s_driver_install(MIC_I2S, &mc, 0, NULL) != ESP_OK ||
      i2s_set_pin(MIC_I2S, &mp)                 != ESP_OK) {
    Serial.println("[mic] init FAILED"); return;
  }
  i2s_zero_dma_buffer(MIC_I2S);
  Serial.println("[mic] ready");
}

void initSpeaker() {
  i2s_config_t sc = {};
  sc.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  sc.sample_rate          = SPK_RATE;
  sc.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  sc.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  sc.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  sc.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  sc.dma_buf_count        = 4;
  sc.dma_buf_len          = 256;
  sc.use_apll             = false;
  sc.tx_desc_auto_clear   = true;

  i2s_pin_config_t sp = {
    .bck_io_num   = SPK_BCLK,
    .ws_io_num    = SPK_LRC,
    .data_out_num = SPK_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };

  if (i2s_driver_install(SPK_I2S, &sc, 0, NULL) != ESP_OK ||
      i2s_set_pin(SPK_I2S, &sp)                 != ESP_OK) {
    Serial.println("[spk] init FAILED"); return;
  }
  Serial.println("[spk] ready");
}

void initMotors() {
  pinMode(MTR_A_FWD, OUTPUT); pinMode(MTR_A_BWD, OUTPUT);
  pinMode(MTR_B_FWD, OUTPUT); pinMode(MTR_B_BWD, OUTPUT);
  stopMotors();
  Serial.println("[mtr] ready");
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.printf("\n[buddy] booting — id: %s\n", cfg.id);

  txQueue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(Frame));

  initCamera();
  initMic();
  initSpeaker();
  initMotors();

  // WiFi
  Serial.printf("[wifi] connecting to \"%s\"\n", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  WiFi.setAutoReconnect(true);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\n[wifi] %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\n[wifi] timed out — will retry");

  // WebSocket
  String path = String("/ws?id=") + cfg.id + "&role=device";
  Serial.printf("[ws]  → %s:%u%s\n", cfg.host, cfg.port, path.c_str());
  ws.begin(cfg.host, (uint16_t)cfg.port, path.c_str());
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);
  ws.enableHeartbeat(15000, 3000, 2);

  xTaskCreatePinnedToCore(cameraTask, "cam", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(micTask,    "mic", 4096, NULL, 1, NULL, 1);

  Serial.println("[buddy] running");
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop
// ─────────────────────────────────────────────────────────────────────────────
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
