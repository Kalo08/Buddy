/*
 * main.c — Buddy v3.0 (ESP32-P4 edition)
 * ─────────────────────────────────────────────────────────────────────────────
 * Target: ESP32-P4 dev board with onboard ES8311 speaker codec and an ESP32-C6
 *         WiFi coprocessor (ESP32-P4-Function-EV-Board, Waveshare ESP32-P4-Nano /
 *         ESP32-P4-WIFI6, …) + an OV5640 camera on MIPI-CSI or DVP.
 *
 * Speaks the same protocol as the Pi / ESP32-CAM, so server.js and the web UI
 * work unchanged:
 *   Connects wss://host/ws?id=BDY-XXXXX&role=device
 *   Sends    binary 0x01 + JPEG          — video (only while a browser is paired)
 *   Sends    text   { "type": "hello", "audio": "pcm16" } on pairing, so the
 *                   browser sends raw PCM instead of opus (see speaker.c)
 *   Receives binary 0x02 + PCM16 mono 16 kHz — hold-to-talk audio
 *   Receives text JSON:
 *     { "type": "client_connected" / "client_disconnected" }
 *     { "type": "cmd",   "dir": "fwd"|"back"|"left"|"right"|"stop" }
 *     { "type": "vec",   "vx": -1..1, "vy": -1..1 }
 *     { "type": "servo", "ch": 0-2, "angle": 0-180 }   (angle < 0 = off)
 *
 * All settings live in `idf.py menuconfig` → Buddy. See README.md.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "buddy.h"

static const char *TAG = "buddy";

// ─── Config block ─────────────────────────────────────────────────────────────
// Same layout + magic as the ESP32-CAM firmware so a flash tool can patch it.
// Defaults come from menuconfig. DO NOT reorder fields.
struct __attribute__((packed)) BuddyConfig {
    uint8_t  magic[8];
    char     ssid[32];
    char     pass[64];
    char     host[40];
    uint32_t port;
    char     id[16];
};
__attribute__((used)) static struct BuddyConfig cfg = {
    .magic = { 0xBD, 0xBD, 0xBD, 0xBD, 0x42, 0x55, 0x44, 0x59 },
    .ssid  = CONFIG_BUDDY_WIFI_SSID,
    .pass  = CONFIG_BUDDY_WIFI_PASS,
    .host  = CONFIG_BUDDY_HUB_HOST,
    .port  = CONFIG_BUDDY_HUB_PORT,
    .id    = CONFIG_BUDDY_ID,
};

volatile bool g_peer_online;

static esp_websocket_client_handle_t s_ws;
static EventGroupHandle_t s_net;
#define NET_GOT_IP BIT0

// ─────────────────────────────────────────────────────────────────────────────
// WiFi (through the ESP32-C6 coprocessor — esp_wifi_remote makes it look local)
// ─────────────────────────────────────────────────────────────────────────────
static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_net, NET_GOT_IP);
        ESP_LOGW(TAG, "[wifi] disconnected — retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "[wifi] " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_net, NET_GOT_IP);
    }
}

static void wifi_start(void)
{
    s_net = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, cfg.ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg.pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // power save adds tens of ms of latency

    ESP_LOGI(TAG, "[wifi] connecting to \"%s\"", cfg.ssid);
    if (!(xEventGroupWaitBits(s_net, NET_GOT_IP, false, true, pdMS_TO_TICKS(20000)) & NET_GOT_IP))
        ESP_LOGW(TAG, "[wifi] timed out — will keep retrying");
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket
// ─────────────────────────────────────────────────────────────────────────────
static void handle_text(const char *txt)
{
    cJSON *doc = cJSON_Parse(txt);
    if (!doc) return;
    const cJSON *jt = cJSON_GetObjectItem(doc, "type");
    const char *t = cJSON_IsString(jt) ? jt->valuestring : "";

    if (strcmp(t, "client_connected") == 0) {
        g_peer_online = true;
        ESP_LOGI(TAG, "[ws]  browser online");
        static const char hello[] = "{\"type\":\"hello\",\"audio\":\"pcm16\",\"rate\":16000}";
        esp_websocket_client_send_text(s_ws, hello, sizeof(hello) - 1, pdMS_TO_TICKS(500));
    } else if (strcmp(t, "client_disconnected") == 0) {
        g_peer_online = false;
        drive_stop();
        ESP_LOGI(TAG, "[ws]  browser offline");
    } else if (strcmp(t, "cmd") == 0) {
        const cJSON *dir = cJSON_GetObjectItem(doc, "dir");
        drive_cmd(cJSON_IsString(dir) ? dir->valuestring : "stop");
    } else if (strcmp(t, "vec") == 0) {
        const cJSON *vx = cJSON_GetObjectItem(doc, "vx");
        const cJSON *vy = cJSON_GetObjectItem(doc, "vy");
        drive_vec(cJSON_IsNumber(vx) ? vx->valuedouble : 0, cJSON_IsNumber(vy) ? vy->valuedouble : 0);
    } else if (strcmp(t, "servo") == 0) {
        const cJSON *ch = cJSON_GetObjectItem(doc, "ch");
        const cJSON *ang = cJSON_GetObjectItem(doc, "angle");
        drive_calibrate(cJSON_IsNumber(ch) ? ch->valueint : 0, cJSON_IsNumber(ang) ? ang->valuedouble : 90);
    }
    cJSON_Delete(doc);
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = data;
    static char txt[512];
    static size_t txt_len;
    static uint8_t bin_type;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "[ws]  connected — id: %s", cfg.id);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        g_peer_online = false;
        drive_stop();
        ESP_LOGW(TAG, "[ws]  disconnected — reconnecting");
        break;

    case WEBSOCKET_EVENT_DATA: {
        // Messages bigger than the client buffer arrive as several DATA
        // events with a growing payload_offset — stitch / stream them.
        bool start = d->payload_offset == 0;
        bool last = d->payload_offset + d->data_len >= d->payload_len;
        if (d->op_code == 0x01) {           // text
            if (start) txt_len = 0;
            size_t n = MIN((size_t)d->data_len, sizeof(txt) - 1 - txt_len);
            memcpy(txt + txt_len, d->data_ptr, n);
            txt_len += n;
            if (last) {
                txt[txt_len] = '\0';
                handle_text(txt);
            }
        } else if (d->op_code == 0x02) {    // binary
            const uint8_t *p = (const uint8_t *)d->data_ptr;
            size_t n = d->data_len;
            if (start) {
                bin_type = n ? p[0] : 0;
                if (n) { p++; n--; }
            }
            if (bin_type == TYPE_AUDIO) speaker_feed(p, n, start);
        }
        break;
    }
    default:
        break;
    }
}

static void ws_start(void)
{
    static char uri[160];
    bool tls = cfg.port == 443;
    snprintf(uri, sizeof(uri), "%s://%s:%lu/ws?id=%s&role=device",
             tls ? "wss" : "ws", cfg.host, (unsigned long)cfg.port, cfg.id);
    ESP_LOGI(TAG, "[ws]  → %s", uri);

    esp_websocket_client_config_t wcfg = {
        .uri                  = uri,
        .crt_bundle_attach    = tls ? esp_crt_bundle_attach : NULL,
        .reconnect_timeout_ms = 3000,
        .network_timeout_ms   = 10000,
        .ping_interval_sec    = 15,   // matches the ESP32/Pi heartbeat
        .buffer_size          = 4096,
        .task_stack           = 8192,
    };
    s_ws = esp_websocket_client_init(&wcfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws, NULL);
    esp_websocket_client_start(s_ws);
}

// Camera callback: prepend the 0x01 type byte and send.
static void on_frame(const uint8_t *jpeg, size_t len)
{
    static uint8_t *tx;
    static size_t tx_cap;
    if (!g_peer_online || !esp_websocket_client_is_connected(s_ws)) return;
    if (len + 1 > tx_cap) {
        uint8_t *nb = heap_caps_realloc(tx, len + 1024, MALLOC_CAP_SPIRAM);
        if (!nb) return;
        tx = nb;
        tx_cap = len + 1024;
    }
    tx[0] = TYPE_VIDEO;
    memcpy(tx + 1, jpeg, len);
    esp_websocket_client_send_bin(s_ws, (const char *)tx, len + 1, pdMS_TO_TICKS(500));
}

// ─────────────────────────────────────────────────────────────────────────────
// Boot
// ─────────────────────────────────────────────────────────────────────────────
void app_main(void)
{
    ESP_LOGI(TAG, "booting — id: %s", cfg.id);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // One I2C bus for both the ES8311 and the camera's SCCB.
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = CONFIG_BUDDY_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_BUDDY_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    drive_init();
    if (speaker_init(bus) != ESP_OK) ESP_LOGW(TAG, "speaker disabled");
    if (camera_init(bus) != ESP_OK) ESP_LOGW(TAG, "camera disabled");

    wifi_start();
    ws_start();
    camera_start(on_frame);

    ESP_LOGI(TAG, "running");
}
