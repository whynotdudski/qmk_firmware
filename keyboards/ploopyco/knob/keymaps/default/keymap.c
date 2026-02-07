/* Copyright 2023 Colin Lam (Ploopy Corporation)
 * Testing: Force click-drag mode (no layers)
 */

#include QMK_KEYBOARD_H
#include "as5600.h"

#ifdef RAW_ENABLE
#include "raw_hid.h"
#endif

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {{{ KC_NO }}};

static uint16_t current_position = 0;
static bool dragging = false;
static uint16_t last_rotation_time = 0;

void keyboard_post_init_user(void) {
    as5600_init();
    current_position = as5600_get_rawangle();
}

report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    uint16_t ra = as5600_get_rawangle();
    int16_t delta = (int16_t)(ra - current_position);
    uint16_t now = timer_read();

    // Wrap into [-2048, 2047]
    if (delta > 2048) {
        delta -= 4096;
    } else if (delta < -2048) {
        delta += 4096;
    }

    // FORCE CLICK-DRAG MODE
    if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
        if (!dragging) {
            mouse_report.buttons |= MOUSE_BTN1;  // Left click down
            dragging = true;
        }
        
        // Move mouse vertically (inverted)
        if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
            mouse_report.y = -delta / POINTING_DEVICE_AS5600_SPEED_DIV;
        } else {
            mouse_report.y = (delta > 0) ? -1 : 1;
        }
        
        current_position = ra;
        last_rotation_time = now;
    }
    
    // Release click after 150ms
    if (dragging && timer_elapsed(last_rotation_time) > 150) {
        mouse_report.buttons &= ~MOUSE_BTN1;
        dragging = false;
    }

    return mouse_report;
}

bool pointing_device_driver_init(void) {
    return true;
}
