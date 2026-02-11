/* Copyright 2025 Ploopy Corporation
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include QMK_KEYBOARD_H
#include "raw_hid.h"

// ─── Ploopy firmware speed modes ──────────────────────────────────────────
// SLOW = Ploopy's default shipped firmware (SPEED_DIV=2, DEADZONE=12)
// FAST = Ploopy's fast firmware (SPEED_DIV=1, DEADZONE=6)
// Note: DEADZONE is enforced in ploopyco.c hardware layer using the #define.
// We can't change it dynamically, so we only switch the division here.
// The effective result: CapsLock ON = ~2x faster, more responsive to light touches.
#define SPEED_DIV_SLOW    2
#define SPEED_DIV_FAST    1

// ─── Conservative acceleration ────────────────────────────────────────────
// 1:1 for most scrolling. Only multiplies at genuinely fast spins.
#define ACCEL_THRESHOLD_FAST  30   // <30ms between ticks = very fast spin → 3x
#define ACCEL_THRESHOLD_MED   70   // <70ms between ticks = moderate spin  → 2x
#define ACCEL_MULT_FAST        3
#define ACCEL_MULT_MED         2
// >70ms (slow/precise scrolling) = 1x, no change

// ─── State ────────────────────────────────────────────────────────────────
static bool     scroll_inverted = true;
static uint32_t last_event_ms   = 0;

extern bool is_drag_scroll;  // suppress linker warning from ploopyco.c

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT(KC_NO)
};

// ─── Scroll logic ─────────────────────────────────────────────────────────
report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    if (mouse_report.v == 0) return mouse_report;

    uint32_t now     = timer_read32();
    uint32_t elapsed = now - last_event_ms;
    last_event_ms    = now;

    // CapsLock ON = fast mode (mirrors Ploopy fast firmware)
    bool fast_mode  = host_keyboard_led_state().caps_lock;
    int8_t speed_div = fast_mode ? SPEED_DIV_FAST : SPEED_DIV_SLOW;

    // Conservative acceleration: only kicks in at fast spin
    int multiplier = 1;
    if      (elapsed < ACCEL_THRESHOLD_FAST) multiplier = ACCEL_MULT_FAST;
    else if (elapsed < ACCEL_THRESHOLD_MED)  multiplier = ACCEL_MULT_MED;

    // Apply division + acceleration
    int8_t val = (mouse_report.v / speed_div) * multiplier;

    // Prevent rounding to zero on slow precise scroll
    if (val == 0 && mouse_report.v != 0)
        val = (mouse_report.v > 0) ? 1 : -1;

    mouse_report.v = scroll_inverted ? -val : val;
    return mouse_report;
}

// ─── Raw HID (Python / Stream Deck) ───────────────────────────────────────
void raw_hid_receive(uint8_t *data, uint8_t length) {
    switch (data[0]) {
        case 0x01: scroll_inverted = !scroll_inverted; break;
        case 0x02: scroll_inverted = false;             break;
        case 0x03: scroll_inverted = true;              break;
    }
    raw_hid_send(data, length);  // echo confirmation
}
