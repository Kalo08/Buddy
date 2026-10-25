/*
 * camera.c — OV5640 video, all in P4 hardware
 *
 *   OV5640 ──(MIPI-CSI + ISP, or DVP)──► RGB565 frame at the sensor's native size
 *          ──PPA (scale / mirror / flip)──► ≤ BUDDY_CAM_WIDTH × BUDDY_CAM_HEIGHT
 *          ──JPEG encoder──► callback (sent as 0x01 + JPEG, like every Buddy)
 *
 * The sensor's capture size is whatever its default format in menuconfig is
 * (Component config → Espressif Camera Sensors Configurations → OV5640); we
 * downscale from there, so any format works as long as it's RGB565-capable.
 */

#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "buddy.h"

static const char *TAG = "cam";

#define CAM_BUF_COUNT 2

static int s_fd = -1;
static uint8_t *s_cam_bufs[CAM_BUF_COUNT];
static uint32_t s_cam_w, s_cam_h;

static ppa_client_handle_t s_ppa;
static ppa_srm_oper_config_t s_srm;
static uint8_t *s_scaled;
static size_t s_scaled_cap;

static jpeg_encoder_handle_t s_jenc;
static jpeg_encode_cfg_t s_jcfg;
static uint8_t *s_jpeg;
static size_t s_jpeg_cap;

static camera_frame_cb_t s_cb;

static esp_err_t init_video_stack(i2c_master_bus_handle_t bus)
{
    // SCCB rides the I2C bus main.c already created (shared with the ES8311).
#if CONFIG_BUDDY_CAM_MIPI
    static esp_video_init_csi_config_t csi = {
        .sccb_config = { .init_sccb = false, .freq = 100000 },
        .reset_pin   = CONFIG_BUDDY_CAM_RESET_GPIO,
        .pwdn_pin    = CONFIG_BUDDY_CAM_PWDN_GPIO,
    };
    csi.sccb_config.i2c_handle = bus;
    esp_video_init_config_t cfg = { .csi = &csi };
#else
    static esp_video_init_dvp_config_t dvp = {
        .sccb_config = { .init_sccb = false, .freq = 100000 },
        .reset_pin   = CONFIG_BUDDY_CAM_RESET_GPIO,
        .pwdn_pin    = CONFIG_BUDDY_CAM_PWDN_GPIO,
        .dvp_pin = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                CONFIG_BUDDY_DVP_D0_GPIO, CONFIG_BUDDY_DVP_D1_GPIO,
                CONFIG_BUDDY_DVP_D2_GPIO, CONFIG_BUDDY_DVP_D3_GPIO,
                CONFIG_BUDDY_DVP_D4_GPIO, CONFIG_BUDDY_DVP_D5_GPIO,
                CONFIG_BUDDY_DVP_D6_GPIO, CONFIG_BUDDY_DVP_D7_GPIO,
            },
            .vsync_io = CONFIG_BUDDY_DVP_VSYNC_GPIO,
            .de_io    = CONFIG_BUDDY_DVP_HREF_GPIO,
            .pclk_io  = CONFIG_BUDDY_DVP_PCLK_GPIO,
            .xclk_io  = CONFIG_BUDDY_DVP_XCLK_GPIO,
        },
        .xclk_freq = CONFIG_BUDDY_DVP_XCLK_HZ,
    };
    if (CONFIG_BUDDY_DVP_PCLK_GPIO < 0 || CONFIG_BUDDY_DVP_D0_GPIO < 0) {
        ESP_LOGE(TAG, "DVP pins not set — menuconfig → Buddy → Camera → DVP pins");
        return ESP_ERR_INVALID_ARG;
    }
    dvp.sccb_config.i2c_handle = bus;
    esp_video_init_config_t cfg = { .dvp = &dvp };
#endif
    return esp_video_init(&cfg);
}

static esp_err_t open_capture(void)
{
#if CONFIG_BUDDY_CAM_MIPI
    const char *dev = ESP_VIDEO_MIPI_CSI_DEVICE_NAME;
#else
    const char *dev = ESP_VIDEO_DVP_DEVICE_NAME;
#endif
    s_fd = open(dev, O_RDONLY);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "open %s failed — sensor not detected?", dev);
        return ESP_FAIL;
    }

    struct v4l2_capability cap = { 0 };
    if (ioctl(s_fd, VIDIOC_QUERYCAP, &cap) == 0)
        ESP_LOGI(TAG, "sensor: %s (%s)", (char *)cap.card, dev);

    // Keep the sensor's native size, ask for RGB565 (the ISP converts for MIPI).
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(s_fd, VIDIOC_G_FMT, &fmt) != 0) return ESP_FAIL;
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) != 0) {
            ESP_LOGE(TAG, "sensor format can't produce RGB565 — pick an RGB565/RAW OV5640 "
                          "format in menuconfig (Espressif Camera Sensors Configurations)");
            return ESP_FAIL;
        }
    }
    s_cam_w = fmt.fmt.pix.width;
    s_cam_h = fmt.fmt.pix.height;

    struct v4l2_requestbuffers req = {
        .count  = CAM_BUF_COUNT,
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) return ESP_FAIL;

    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index  = i,
        };
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &buf) != 0) return ESP_FAIL;
        s_cam_bufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, buf.m.offset);
        if (!s_cam_bufs[i] || s_cam_bufs[i] == MAP_FAILED) return ESP_FAIL;
        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) return ESP_FAIL;
    }
    return ESP_OK;
}

// Pick a uniform PPA scale (1/16 steps) that fits the target box, and
// center-crop the input so the output is a multiple of 16 on both sides —
// the JPEG encoder's 4:2:0 macroblocks need that.
static esp_err_t init_scaler_and_encoder(void)
{
    float s = fminf((float)CONFIG_BUDDY_CAM_WIDTH / s_cam_w, (float)CONFIG_BUDDY_CAM_HEIGHT / s_cam_h);
    s = fminf(s, 1.0f);
    s = fmaxf(floorf(s * 16.0f) / 16.0f, 1.0f / 16.0f);

    uint32_t out_w = ((uint32_t)(s_cam_w * s)) & ~15u;
    uint32_t out_h = ((uint32_t)(s_cam_h * s)) & ~15u;
    uint32_t blk_w = MIN(s_cam_w, (uint32_t)(out_w / s));
    uint32_t blk_h = MIN(s_cam_h, (uint32_t)(out_h / s));

    jpeg_encode_memory_alloc_cfg_t in_mem  = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t out_mem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    s_scaled = jpeg_alloc_encoder_mem(out_w * out_h * 2, &in_mem, &s_scaled_cap);
    s_jpeg   = jpeg_alloc_encoder_mem(out_w * out_h, &out_mem, &s_jpeg_cap);
    if (!s_scaled || !s_jpeg) return ESP_ERR_NO_MEM;

    ppa_client_config_t pcfg = { .oper_type = PPA_OPERATION_SRM };
    ESP_ERROR_CHECK(ppa_register_client(&pcfg, &s_ppa));
    s_srm = (ppa_srm_oper_config_t){
        .in = {
            .pic_w = s_cam_w, .pic_h = s_cam_h,
            .block_w = blk_w, .block_h = blk_h,
            .block_offset_x = (s_cam_w - blk_w) / 2,
            .block_offset_y = (s_cam_h - blk_h) / 2,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_scaled, .buffer_size = s_scaled_cap,
            .pic_w = out_w, .pic_h = out_h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x  = s,
        .scale_y  = s,
        .mirror_x = CONFIG_BUDDY_CAM_MIRROR,
        .mirror_y = CONFIG_BUDDY_CAM_FLIP,
        .byte_swap = CONFIG_BUDDY_CAM_SWAP_BYTES,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    jpeg_encode_engine_cfg_t ecfg = { .timeout_ms = 100 };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&ecfg, &s_jenc));
    s_jcfg = (jpeg_encode_cfg_t){
        .width         = out_w,
        .height        = out_h,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = CONFIG_BUDDY_CAM_JPEG_QUALITY,
    };

    ESP_LOGI(TAG, "ready — capture %lux%lu → stream %lux%lu @ %d fps, q%d",
             s_cam_w, s_cam_h, out_w, out_h, CONFIG_BUDDY_CAM_FPS, CONFIG_BUDDY_CAM_JPEG_QUALITY);
    return ESP_OK;
}

esp_err_t camera_init(i2c_master_bus_handle_t bus)
{
    esp_err_t err = init_video_stack(bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video init failed (%s) — check the camera cable", esp_err_to_name(err));
        return err;
    }
    if ((err = open_capture()) != ESP_OK) return err;
    return init_scaler_and_encoder();
}

static size_t encode(const uint8_t *frame)
{
    s_srm.in.buffer = frame;
    if (ppa_do_scale_rotate_mirror(s_ppa, &s_srm) != ESP_OK) return 0;
    uint32_t out_len = 0;
    if (jpeg_encoder_process(s_jenc, &s_jcfg, s_scaled, s_jcfg.width * s_jcfg.height * 2,
                             s_jpeg, s_jpeg_cap, &out_len) != ESP_OK) return 0;
    return out_len;
}

static void cam_task(void *arg)
{
    const int64_t period_us = 1000000 / CONFIG_BUDDY_CAM_FPS;
    int64_t next_us = 0;
    for (;;) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGW(TAG, "DQBUF failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t len = 0;
        int64_t now = esp_timer_get_time();
        if (g_peer_online && now >= next_us) {
            next_us = now + period_us;
            len = encode(s_cam_bufs[buf.index]);
        }
        // Hand the buffer straight back — sending can be slow, the sensor shouldn't wait on it.
        ioctl(s_fd, VIDIOC_QBUF, &buf);
        if (len) s_cb(s_jpeg, len);
    }
}

void camera_start(camera_frame_cb_t cb)
{
    if (s_fd < 0 || !s_jenc) return;
    s_cb = cb;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON failed");
        return;
    }
    xTaskCreatePinnedToCore(cam_task, "cam", 6144, NULL, 5, NULL, 1);
}
