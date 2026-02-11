/* Copyright 2023 Colin Lam (Ploopy Corporation)
 * Inverted scroll + velocity acceleration + CapsLock fast mode
 */

#include QMK_KEYBOARD_H
#include "as5600.h"

#ifdef RAW_ENABLE
#include "raw_hid.h"
#endif

// Layers
enum layers {
    _SCROLL = 0,
    _ABLETON
};

// ===== TUNE THESE =====
// Thresholds: time in ms between rotation events (smaller = faster spin)
// Stays at 1x for anything slower than FAST threshold
#define ACCEL_THRESHOLD_FAST  60   // ms — below this = 2x
#define ACCEL_THRESHOLD_VFAST 25   // ms — below this = 4x

// Slow mode (CapsLock OFF): normal base, gentle acceleration
#define ACCEL_SLOW_FAST  2   // 2x at fast spin
#define ACCEL_SLOW_VFAST 4   // 4x at very fast spin

// Fast mode (CapsLock ON): same thresholds, bigger multipliers
#define ACCEL_FAST_FAST  4   // 4x at fast spin
#define ACCEL_FAST_VFAST 8   // 8x at very fast spin
// ======================

#define DRAG_RELEASE_TIMEOUT 400

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {{{ KC_NO }}};

static uint16_t current_position = 0;
static bool dragging = false;
static uint16_t last_rotation_time = 0;

void keyboard_post_init_user(void) {
    as5600_init();
    current_position = as5600_get_rawangle();
}

#ifdef RAW_ENABLE
void raw_hid_receive(uint8_t *data, uint8_t length) {
    switch (data[0]) {
        case 0x01: layer_invert(_ABLETON); break;
        case 0x02: layer_off(_ABLETON);    break;
        case 0x03: layer_on(_ABLETON);     break;
    }
}
#endif

report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    uint16_t ra    = as5600_get_rawangle();
    int16_t  delta = (int16_t)(ra - current_position);
    uint16_t now   = timer_read();

    if (delta > 2048)       delta -= 4096;
    else if (delta < -2048) delta += 4096;

    bool caps = host_keyboard_led_state().caps_lock;

    uint16_t time_since_last = timer_elapsed(last_rotation_time);
    int16_t speed_multiplier;

    if (time_since_last < ACCEL_THRESHOLD_VFAST) {
        speed_multiplier = caps ? ACCEL_FAST_VFAST : ACCEL_SLOW_VFAST;
    } else if (time_since_last < ACCEL_THRESHOLD_FAST) {
        speed_multiplier = caps ? ACCEL_FAST_FAST : ACCEL_SLOW_FAST;
    } else {
        speed_multiplier = caps ? 2 : 1;  // fast mode has 2x baseline
    }

    if (IS_LAYER_ON(_ABLETON)) {
        if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
            if (!dragging) {
                mouse_report.buttons |= MOUSE_BTN1;
                dragging = true;
            }
            if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
                mouse_report.y = (delta * speed_multiplier) / POINTING_DEVICE_AS5600_SPEED_DIV;
            } else {
                mouse_report.y = (delta > 0) ? speed_multiplier : -speed_multiplier;
            }
            current_position = ra;
            last_rotation_time = now;
        }
        if (dragging && timer_elapsed(last_rotation_time) > DRAG_RELEASE_TIMEOUT) {
            mouse_report.buttons &= ~MOUSE_BTN1;
            dragging = false;
        }
    } else {
        if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
            if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
                current_position = ra;
                mouse_report.v = (-delta * speed_multiplier) / POINTING_DEVICE_AS5600_SPEED_DIV;
            }
        } else {
            if (delta >= POINTING_DEVICE_AS5600_TICK_COUNT) {
                current_position = ra;
                mouse_report.v = -speed_multiplier;
            } else if (delta <= -POINTING_DEVICE_AS5600_TICK_COUNT) {
                current_position = ra;
                mouse_report.v = speed_multiplier;
            }
        }
    }

    return mouse_report;
}

bool pointing_device_driver_init(void) {
    return true;
}
