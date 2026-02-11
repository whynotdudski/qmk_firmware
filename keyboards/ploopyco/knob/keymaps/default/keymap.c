/* Copyright 2023 Colin Lam (Ploopy Corporation)
 * Inverted scroll + conservative acceleration + CapsLock fast/slow toggle
 */

#include QMK_KEYBOARD_H
#include "as5600.h"

#ifdef RAW_ENABLE
#include "raw_hid.h"
#endif

// ===== ACCELERATION SETTINGS =====
// Stays at 1x until you spin faster than 50ms between events
// Gentle 2x at moderate spin, 3x only at very fast spin
#define ACCEL_THRESHOLD_VFAST 15   // <15ms between events = 3x (very fast spin)
#define ACCEL_THRESHOLD_FAST  50   // <50ms = 2x (moderate spin)
// anything slower = 1x (normal)

#define ACCEL_MULT_VFAST 3
#define ACCEL_MULT_FAST  2

// CapsLock OFF = slow mode (divide by 2, mirrors Ploopy slow firmware)
// CapsLock ON  = fast mode (divide by 1, mirrors Ploopy fast firmware)
#define SPEED_DIV_SLOW 2
#define SPEED_DIV_FAST 1

#define DRAG_RELEASE_TIMEOUT 400
// ==================================

enum layers {
    _SCROLL = 0,
    _ABLETON
};

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
    uint16_t ra = as5600_get_rawangle();
    int16_t delta = (int16_t)(ra - current_position);
    uint16_t now = timer_read();

    if (delta > 2048)       delta -= 4096;
    else if (delta < -2048) delta += 4096;

    // CapsLock = fast mode
    int8_t speed_div = host_keyboard_led_state().caps_lock ? SPEED_DIV_FAST : SPEED_DIV_SLOW;

    // Conservative acceleration: 1x most of the time
    uint16_t time_since_last = timer_elapsed(last_rotation_time);
    int16_t speed_multiplier;

    if      (time_since_last < ACCEL_THRESHOLD_VFAST) speed_multiplier = ACCEL_MULT_VFAST;
    else if (time_since_last < ACCEL_THRESHOLD_FAST)  speed_multiplier = ACCEL_MULT_FAST;
    else                                               speed_multiplier = 1;

    if (IS_LAYER_ON(_ABLETON)) {
        if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
            if (!dragging) {
                mouse_report.buttons |= MOUSE_BTN1;
                dragging = true;
            }
            if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
                mouse_report.y = (delta * speed_multiplier) / (POINTING_DEVICE_AS5600_SPEED_DIV * speed_div);
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
                mouse_report.v = (-delta * speed_multiplier) / (POINTING_DEVICE_AS5600_SPEED_DIV * speed_div);
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
