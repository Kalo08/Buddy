/*
 * drive.c — wheel servos, ported from pi/buddy_pi.py
 *
 * Same chassis, same math as the Pi: three pot-decoupled continuous-rotation
 * servos, two in front and one sideways in back. Same rule too: at idle the
 * servos get NO signal at all (duty 0, pin held low) — pulses only flow while
 * a drive command is held. The difference is the pulses now come from the
 * P4's LEDC hardware PWM instead of RPi.GPIO software PWM, so they're
 * jitter-free — which also means the NEUTRAL_US values measured on the Pi may
 * be a few µs off here. Re-check them (see README).
 */

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "buddy.h"

static const char *TAG = "srv";

#define NUM_SERVOS   3
#define SERVO_FREQ   50
#define DUTY_BITS    LEDC_TIMER_14_BIT
#define PERIOD_US    (1000000 / SERVO_FREQ)

// [front-left (ch0), front-right (ch1), back (ch2)]
static const int SERVO_PINS[NUM_SERVOS] = {
    CONFIG_BUDDY_SERVO_FL_GPIO, CONFIG_BUDDY_SERVO_FR_GPIO, CONFIG_BUDDY_SERVO_BACK_GPIO,
};

// Pulse matching each servo's frozen pot — the midpoint drive commands offset
// from. Measured on the Pi; recalibrate with drive_calibrate() if a wheel creeps.
static float NEUTRAL_US[NUM_SERVOS] = { 1489, 1494, 1494 };

#define DRIVE_US   70.0f   // offset from neutral at 100% commanded speed
#define DEADBAND   0.05f   // |speed| below this cuts the channel entirely
#define STEER_MIX  0.5f    // sideways component scale while also driving fwd/back

// Stall-protection workaround: the servo firmware cuts power after ~5 s of a
// never-shrinking position error. Drive in REST_ON_MS windows with a short
// neutral-pulse rest between them ("target reached" resets the detector).
#define REST_ON_MS   4000
#define REST_OFF_MS  50

// Drive geometry — wheel drive direction in the (forward, right) frame.
static const float WHEEL_FWD[NUM_SERVOS]   = { 0.8660254f, -0.8660254f,  0.0f };
static const float WHEEL_RIGHT[NUM_SERVOS] = { 0.5f,        0.5f,       -1.0f };
static const float WHEEL_DIR[NUM_SERVOS]   = { -1.0f, -1.0f, 1.0f };  // both fronts mounted reversed
static const float WHEEL_GAIN[NUM_SERVOS]  = { 1.0f, 1.0f, 1.0f };

// FL drags → nose pulls left on fwd/back; the back wheel counter-steers.
// Flip the sign if it makes the pull worse. 0 disables.
#define BACK_ASSIST 0.15f

static float s_targets[NUM_SERVOS];   // commanded speed per channel, 0 = off
static SemaphoreHandle_t s_lock;

static void write_us(int ch, float us)
{
    uint32_t duty = (uint32_t)(us * (1 << DUTY_BITS) / PERIOD_US);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch);
    ESP_LOGD(TAG, "ch%d → %.0fµs", ch, us);
}

static void write_off(int ch)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch);
}

static void set_speed(int ch, float speed)
{
    if (speed > 1.0f) speed = 1.0f;
    if (speed < -1.0f) speed = -1.0f;
    if (fabsf(speed) < DEADBAND) speed = 0.0f;
    s_targets[ch] = speed;
    if (speed == 0.0f) write_off(ch);
    else write_us(ch, NEUTRAL_US[ch] + speed * DRIVE_US);
}

static void drive(float vx, float vy)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < NUM_SERVOS; i++) {
        float speed = WHEEL_DIR[i] * (WHEEL_FWD[i] * vx + WHEEL_RIGHT[i] * vy);
        if (i == 2) speed += WHEEL_DIR[2] * BACK_ASSIST * vx;  // counter the FL drag
        set_speed(i, WHEEL_GAIN[i] * speed);
    }
    xSemaphoreGive(s_lock);
}

static bool any_active(void)
{
    for (int i = 0; i < NUM_SERVOS; i++)
        if (s_targets[i] != 0.0f) return true;
    return false;
}

// Rest cycling: every REST_ON_MS of continuous driving, hold neutral for
// REST_OFF_MS, then resume whatever is still commanded.
static void rest_task(void *arg)
{
    const int step_ms = 100;
    int driven_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        if (!any_active()) {
            driven_ms = 0;
            continue;
        }
        driven_ms += step_ms;
        if (driven_ms < REST_ON_MS) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < NUM_SERVOS; i++)
            if (s_targets[i] != 0.0f) write_us(i, NEUTRAL_US[i]);
        xSemaphoreGive(s_lock);

        vTaskDelay(pdMS_TO_TICKS(REST_OFF_MS));
        driven_ms = 0;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < NUM_SERVOS; i++)
            if (s_targets[i] != 0.0f) write_us(i, NEUTRAL_US[i] + s_targets[i] * DRIVE_US);
        xSemaphoreGive(s_lock);
    }
}

void drive_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = DUTY_BITS,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = SERVO_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    for (int i = 0; i < NUM_SERVOS; i++) {
        ledc_channel_config_t ch = {
            .gpio_num   = SERVO_PINS[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = (ledc_channel_t)i,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = 0,   // no pulses until driven
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }

    xTaskCreate(rest_task, "rest", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "LEDC servos ready (GPIO %d/%d/%d) — all outputs off until driven",
             SERVO_PINS[0], SERVO_PINS[1], SERVO_PINS[2]);
}

void drive_cmd(const char *dir)
{
    if      (strcmp(dir, "fwd") == 0)   drive( 1,  0);
    else if (strcmp(dir, "back") == 0)  drive(-1,  0);
    else if (strcmp(dir, "left") == 0)  drive( 0, -1);
    else if (strcmp(dir, "right") == 0) drive( 0,  1);
    else                                drive( 0,  0);  // stop / unknown
}

void drive_vec(float vx, float vy)
{
    vx = fmaxf(-1.0f, fminf(1.0f, vx));
    vy = fmaxf(-1.0f, fminf(1.0f, vy));
    if (vx != 0.0f && vy != 0.0f) vy *= STEER_MIX;  // soften steering while driving
    // Normalize diagonals so no wheel gets clipped at the clamp.
    float mag = sqrtf(vx * vx + vy * vy);
    if (mag > 1.0f) {
        vx /= mag;
        vy /= mag;
    }
    drive(vx, vy);
}

// { "type": "servo", "ch": 0-2, "angle": 0-180 } — maps 0-180 onto
// 500-2500µs (90 = 1500µs) and logs the µs. When a wheel stops, put that
// value into NEUTRAL_US. angle < 0 cuts the channel.
void drive_calibrate(int ch, float angle)
{
    if (ch < 0 || ch >= NUM_SERVOS) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (angle < 0) {
        write_off(ch);
    } else {
        float us = 500.0f + fminf(180.0f, angle) * 2000.0f / 180.0f;
        ESP_LOGI(TAG, "ch%d calibrate → %.0fµs (angle %.1f)", ch, us, angle);
        write_us(ch, us);
    }
    xSemaphoreGive(s_lock);
}

void drive_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < NUM_SERVOS; i++) {
        s_targets[i] = 0.0f;
        write_off(i);
    }
    xSemaphoreGive(s_lock);
}
