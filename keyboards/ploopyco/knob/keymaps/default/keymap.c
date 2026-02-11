/* Copyright 2025 Ploopy Corporation
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include QMK_KEYBOARD_H
#include "raw_hid.h"

#define SPEED_DIV_SLOW    2
#define SPEED_DIV_FAST    1

#define ACCEL_THRESHOLD_FAST  30
#define ACCEL_THRESHOLD_MED   70
#define ACCEL_MULT_FAST        3
#define ACCEL_MULT_MED         2

static bool     scroll_inverted = true;
static uint32_t last_event_ms   = 0;

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT()
};

report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    if (mouse_report.v == 0) return mouse_report;

    uint32_t now     = timer_read32();
    uint32_t elapsed = now - last_event_ms;
    last_event_ms    = now;

    bool fast_mode   = host_keyboard_led_state().caps_lock;
    int8_t speed_div = fast_mode ? SPEED_DIV_FAST : SPEED_DIV_SLOW;

    int multiplier = 1;
    if      (elapsed < ACCEL_THRESHOLD_FAST) multiplier = ACCEL_MULT_FAST;
    else if (elapsed < ACCEL_THRESHOLD_MED)  multiplier = ACCEL_MULT_MED;

    int8_t val = (mouse_report.v / speed_div) * multiplier;

    if (val == 0 && mouse_report.v != 0)
        val = (mouse_report.v > 0) ? 1 : -1;

    mouse_report.v = scroll_inverted ? -val : val;
    return mouse_report;
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    switch (data[0]) {
        case 0x01: scroll_inverted = !scroll_inverted; break;
        case 0x02: scroll_inverted = false;             break;
        case 0x03: scroll_inverted = true;              break;
    }
    raw_hid_send(data, length);
}
