#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// ─── Frame type bytes (same as every other Buddy device) ──────────────────────
#define TYPE_VIDEO 0x01
#define TYPE_AUDIO 0x02

// True while a browser is paired — video is only encoded/sent then.
extern volatile bool g_peer_online;

// ─── drive.c — 3 continuous-rotation wheel servos on LEDC ────────────────────
void drive_init(void);
void drive_cmd(const char *dir);         // "fwd" | "back" | "left" | "right" | "stop"
void drive_vec(float vx, float vy);      // combined-key driving, -1..1 each
void drive_calibrate(int ch, float angle); // raw pulse, angle < 0 = output off
void drive_stop(void);

// ─── speaker.c — PCM16 mono 16 kHz from the browser → ES8311 ──────────────────
esp_err_t speaker_init(i2c_master_bus_handle_t bus);
// Feed one websocket fragment of audio. frame_start = first fragment of a new
// websocket message. Never blocks — drops audio if the player is behind.
void speaker_feed(const uint8_t *data, size_t len, bool frame_start);

// ─── camera.c — OV5640 → PPA downscale → hardware JPEG ────────────────────────
typedef void (*camera_frame_cb_t)(const uint8_t *jpeg, size_t len);
esp_err_t camera_init(i2c_master_bus_handle_t bus);
void camera_start(camera_frame_cb_t cb);
