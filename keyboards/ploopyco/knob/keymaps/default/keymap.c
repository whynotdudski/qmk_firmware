/* Copyright 2023 Colin Lam (Ploopy Corporation)
 * Inverted scroll + acceleration + ScrollLock drag scroll toggle
 * Configuration: adapted_1
 */

#include QMK_KEYBOARD_H
#include "as5600.h"

#ifdef RAW_ENABLE
#include "raw_hid.h"
#endif

extern bool is_drag_scroll;  // Access drag scroll state from ploopyco.c

// ===== ACCELERATION SETTINGS - ADJUST THESE VALUES =====
#define ACCEL_THRESHOLD_VFAST 20
#define ACCEL_THRESHOLD_FAST  40
#define ACCEL_THRESHOLD_MED   80
#define ACCEL_THRESHOLD_SLOW  150

#define ACCEL_MULT_VFAST 5
#define ACCEL_MULT_FAST  3
#define ACCEL_MULT_MED   2
#define ACCEL_MULT_SLOW  1
// =======================================================

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {{{ KC_NO }}};

static uint16_t current_position = 0;
static uint16_t last_rotation_time = 0;

void keyboard_post_init_user(void) {
    as5600_init();
    current_position = as5600_get_rawangle();
}

#ifdef RAW_ENABLE
void raw_hid_receive(uint8_t *data, uint8_t length) {
    switch (data[0]) {
        case 0x01:
            toggle_drag_scroll();
            break;
        case 0x02:
            if (is_drag_scroll) toggle_drag_scroll();
            break;
        case 0x03:
            if (!is_drag_scroll) toggle_drag_scroll();
            break;
    }
}
#endif

report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    uint16_t ra = as5600_get_rawangle();
    int16_t delta = (int16_t)(ra - current_position);
    uint16_t now = timer_read();

    // Wrap into [-2048, 2047]
    if (delta > 2048) delta -= 4096;
    else if (delta < -2048) delta += 4096;

    // ScrollLock controls drag scroll mode
    bool scrolllock = host_keyboard_led_state().scroll_lock;
    if (scrolllock && !is_drag_scroll) toggle_drag_scroll();
    else if (!scrolllock && is_drag_scroll) toggle_drag_scroll();

    // Acceleration
    uint16_t time_since_last = timer_elapsed(last_rotation_time);
    int16_t speed_multiplier;

    if (time_since_last < ACCEL_THRESHOLD_VFAST)
        speed_multiplier = ACCEL_MULT_VFAST;
    else if (time_since_last < ACCEL_THRESHOLD_FAST)
        speed_multiplier = ACCEL_MULT_FAST;
    else if (time_since_last < ACCEL_THRESHOLD_MED)
        speed_multiplier = ACCEL_MULT_MED;
    else if (time_since_last < ACCEL_THRESHOLD_SLOW)
        speed_multiplier = ACCEL_MULT_SLOW;
    else
        speed_multiplier = 1;

    // Apply inverted scroll with acceleration
    if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
        if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
            mouse_report.v = (-delta * speed_multiplier) / POINTING_DEVICE_AS5600_SPEED_DIV;
        } else {
            mouse_report.v = (delta > 0) ? -speed_multiplier : speed_multiplier;
        }
        current_position = ra;
        last_rotation_time = now;
    }

    return mouse_report;
}

bool pointing_device_driver_init(void) {
    return true;
}
