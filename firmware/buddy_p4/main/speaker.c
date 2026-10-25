/*
 * speaker.c — hold-to-talk audio from the browser, out the ES8311 codec
 *
 * The Pi decodes the browser's MediaRecorder opus with ffplay; the P4 can't
 * cheaply, so this device announces { "type": "hello", "audio": "pcm16" } and
 * client.js sends raw PCM16 mono @ 16 kHz instead. Audio lands in a stream
 * buffer from the websocket task and a separate task plays it, so a slow
 * codec can never stall the websocket (same rule as the Pi's speaker thread).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "buddy.h"

static const char *TAG = "spk";

#define SPK_RATE      16000
#define BUF_BYTES     (SPK_RATE * 2)          // 1 s of audio, max lag before dropping
#define PREBUF_BYTES  (SPK_RATE * 2 / 10)     // 100 ms cushion before playback starts

static StreamBufferHandle_t s_buf;
static esp_codec_dev_handle_t s_spk;
static bool s_ready;

// Websocket fragments can split a 16-bit sample in half. Only ever push
// whole samples into the buffer (carrying a stray byte to the next fragment),
// so dropping a piece on overflow can never knock the stream out of alignment.
static uint8_t s_carry;
static bool s_has_carry;

static void push(const uint8_t *d, size_t n)
{
    if (xStreamBufferSpacesAvailable(s_buf) < n) return;  // player behind — drop
    xStreamBufferSend(s_buf, d, n, 0);
}

void speaker_feed(const uint8_t *data, size_t len, bool frame_start)
{
    if (!s_ready) return;
    if (frame_start) s_has_carry = false;
    if (s_has_carry && len) {
        uint8_t pair[2] = { s_carry, data[0] };
        push(pair, 2);
        data++;
        len--;
        s_has_carry = false;
    }
    if (len & 1) {
        s_carry = data[len - 1];
        s_has_carry = true;
        len--;
    }
    if (len) push(data, len);
}

static void play_task(void *arg)
{
    static uint8_t chunk[512];
    bool playing = false;
    int idle_ms = 0;
    for (;;) {
        if (!playing) {
            size_t avail = xStreamBufferBytesAvailable(s_buf);
            // Wait for a cushion, unless a short clip has been sitting a while.
            if (avail < PREBUF_BYTES && !(avail && idle_ms >= 200)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                idle_ms += 10;
                continue;
            }
            playing = true;
            idle_ms = 0;
        }
        size_t n = xStreamBufferReceive(s_buf, chunk, sizeof(chunk), pdMS_TO_TICKS(60));
        if (n == 0) {
            playing = false;  // underrun — rebuild the cushion
            continue;
        }
        esp_codec_dev_write(s_spk, chunk, n);  // blocks at real-time rate
    }
}

esp_err_t speaker_init(i2c_master_bus_handle_t bus)
{
#if !CONFIG_BUDDY_SPK_ENABLE
    ESP_LOGI(TAG, "disabled in menuconfig");
    return ESP_OK;
#else
    i2s_chan_handle_t tx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // silence, not a looping buzz, on underrun
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, NULL), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = CONFIG_BUDDY_I2S_MCLK_GPIO,
            .bclk = CONFIG_BUDDY_I2S_BCLK_GPIO,
            .ws   = CONFIG_BUDDY_I2S_WS_GPIO,
            .dout = CONFIG_BUDDY_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s enable");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = I2C_NUM_0,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = CONFIG_BUDDY_SPK_PA_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk    = true,
        .hw_gain     = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!data_if || !ctrl_if || !codec_if) {
        ESP_LOGE(TAG, "ES8311 not found on I2C — check SDA/SCL pins");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_spk = esp_codec_dev_new(&dev_cfg);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = SPK_RATE,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk, CONFIG_BUDDY_SPK_VOLUME);

    s_buf = xStreamBufferCreate(BUF_BYTES, 2);
    xTaskCreate(play_task, "spk", 4096, NULL, 6, NULL);
    s_ready = true;
    ESP_LOGI(TAG, "ES8311 ready — PCM16 mono %d Hz, vol %d", SPK_RATE, CONFIG_BUDDY_SPK_VOLUME);
    return ESP_OK;
#endif
}
