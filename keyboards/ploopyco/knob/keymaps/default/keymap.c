/* Copyright 2023 Colin Lam (Ploopy Corporation)
 * Eden's custom firmware — delta-based acceleration
 *
 * HOW IT WORKS:
 *   QMK polls the AS5600 magnetic sensor at a fixed rate.
 *   Each poll gives delta = angle change since last poll.
 *   Fast spin = large delta. Slow spin = small delta.
 *   Original firmware: mouse_report.v = -delta / SPEED_DIV  (fixed 1× gain)
 *   This firmware:     mouse_report.v = -delta * mult(|delta|) / (SPEED_DIV * 16)
 *   When mult=16, output is identical to original. mult>16 = faster.
 *   The curve maps |delta| → multiplier, tunable via GUI without reflashing.
 *
 * HID COMMANDS:
 *   0x01  = toggle Ableton layer
 *   0x02  = force normal layer
 *   0x03  = force Ableton layer
 *   0x10  = update curve from GUI
 *   0x11  = query last abs(delta) for GUI live monitor dot
 *
 * CURVE PACKET FORMAT (0x10):
 *   [cmd=0x10, count, d_lo, d_hi, slow_mult, fast_mult, ...]
 *   Points sorted ascending by delta (slowest first).
 *   slow/fast are ×16 fixed-point: value 16 = 1.0×, 32 = 2.0×, etc.
 */

#include QMK_KEYBOARD_H
#include "as5600.h"

#ifdef RAW_ENABLE
#include "raw_hid.h"
#endif

enum layers { _SCROLL = 0, _ABLETON };

// ─── Curve ────────────────────────────────────────────────────────────────────
// X = abs(delta): how many angle units moved in one sensor poll period.
// Observed range: ~1–2 for careful slow turn, ~30–60 for fast spin.
// Y = multiplier ×16 (16 = 1×, 32 = 2×, 48 = 3×, etc.)

#define MAX_CURVE_POINTS 8

static uint16_t curve_deltas[MAX_CURVE_POINTS] = {
     2,   // gentle: up to here = 1× (identical to original firmware)
     5,   // picking up
    10,   // moderate
    20,   // fast
    35,   // very fast
    55,   // max spin
     0,
     0
};

// Normal mode (CapsLock OFF), ×16 fixed-point
static uint8_t slow_mults[MAX_CURVE_POINTS] = {
    16,   // 1.0× — precise, no boost
    20,   // 1.25×
    28,   // 1.75×
    48,   // 3×
    80,   // 5×
   112,   // 7×
   112,
   112
};

// Fast mode (CapsLock ON), ×16 fixed-point
static uint8_t fast_mults[MAX_CURVE_POINTS] = {
    24,   // 1.5× even at rest
    40,   // 2.5×
    72,   // 4.5×
   112,   // 7×
   160,   // 10×
   224,   // 14×
   224,
   224
};

static uint8_t curve_point_count = 6;

// ─── State ────────────────────────────────────────────────────────────────────
static uint16_t current_position  = 0;
static bool     dragging          = false;
static uint16_t last_move_time    = 0;

// GUI live dot: accumulate abs(delta) over a rolling 50ms window
// so the reported value reflects actual scroll speed, not a single poll
static uint32_t delta_accum       = 0;  // sum of abs(delta) in current window
static uint16_t delta_window_start = 0; // timer value when window opened
static uint16_t delta_reported    = 0;  // last completed window sum

#define DRAG_RELEASE_TIMEOUT 400

void keyboard_post_init_user(void) {
    as5600_init();
    current_position = as5600_get_rawangle();
}

// ─── HID ──────────────────────────────────────────────────────────────────────
#ifdef RAW_ENABLE
void raw_hid_receive(uint8_t *data, uint8_t length) {
    switch (data[0]) {
        case 0x01: layer_invert(_ABLETON); break;
        case 0x02: layer_off(_ABLETON);    break;
        case 0x03: layer_on(_ABLETON);     break;

        case 0x11: {
            uint8_t resp[32] = {0};
            resp[0] = 0x11;
            resp[1] = delta_reported & 0xFF;
            resp[2] = (delta_reported >> 8) & 0xFF;
            raw_hid_send(resp, 32);
            break;
        }

        case 0x10: {
            uint8_t count = data[1];
            if (count < 2 || count > MAX_CURVE_POINTS) break;
            for (uint8_t i = 0; i < count; i++) {
                uint8_t o = 2 + i * 4;
                if (o + 3 >= length) break;
                curve_deltas[i] = data[o] | ((uint16_t)data[o+1] << 8);
                slow_mults[i]   = data[o+2];
                fast_mults[i]   = data[o+3];
            }
            for (uint8_t i = count; i < MAX_CURVE_POINTS; i++) {
                curve_deltas[i] = 0;
                slow_mults[i]   = slow_mults[count-1];
                fast_mults[i]   = fast_mults[count-1];
            }
            curve_point_count = count;
            break;
        }
    }
}
#endif

// ─── Multiplier (×16 fixed-point) ────────────────────────────────────────────
static int16_t get_mult_fp(uint8_t abs_d, bool caps) {
    uint8_t *mults = caps ? fast_mults : slow_mults;

    if (abs_d <= curve_deltas[0])
        return (int16_t)mults[0];

    for (uint8_t i = 0; i < curve_point_count - 1; i++) {
        uint16_t d_lo = curve_deltas[i];
        uint16_t d_hi = curve_deltas[i+1];
        if (d_hi == 0) break;
        if (abs_d >= d_lo && abs_d < d_hi) {
            uint16_t frac16 = ((uint16_t)(abs_d - d_lo) * 16) / (d_hi - d_lo);
            return (int16_t)mults[i] + (int16_t)(frac16 * ((int16_t)mults[i+1] - (int16_t)mults[i])) / 16;
        }
    }

    return (int16_t)mults[curve_point_count - 1];
}

// ─── Sensor task ──────────────────────────────────────────────────────────────
report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    uint16_t ra    = as5600_get_rawangle();
    int16_t  delta = (int16_t)(ra - current_position);

    if (delta > 2048)       delta -= 4096;
    else if (delta < -2048) delta += 4096;

    bool caps = host_keyboard_led_state().caps_lock;

    if (IS_LAYER_ON(_ABLETON)) {
        if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
            uint8_t abs_d  = (uint8_t)(delta < 0 ? -delta : delta);
            int16_t mult   = get_mult_fp(abs_d, caps);
            delta_accum += abs_d;
            if (!dragging) { mouse_report.buttons |= MOUSE_BTN1; dragging = true; }
            if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
                mouse_report.y = (int8_t)((-delta * mult) / (POINTING_DEVICE_AS5600_SPEED_DIV * 16));
            } else {
                mouse_report.y = (delta > 0) ? -(int8_t)(mult/16) : (int8_t)(mult/16);
            }
            current_position = ra;
            last_move_time   = timer_read();
        }
        if (dragging && timer_elapsed(last_move_time) > DRAG_RELEASE_TIMEOUT) {
            mouse_report.buttons &= ~MOUSE_BTN1;
            dragging = false;
        }

    } else {
        if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
            if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
                uint8_t abs_d = (uint8_t)(delta < 0 ? -delta : delta);
                int16_t mult  = get_mult_fp(abs_d, caps);
                current_position = ra;
                mouse_report.v = (int8_t)((-delta * mult) / (POINTING_DEVICE_AS5600_SPEED_DIV * 16));

                // Accumulate delta for GUI speed reporting
                delta_accum += abs_d;
            }
            // Every 50ms: snapshot the accumulator and reset
            if (timer_elapsed(delta_window_start) >= 50) {
                delta_reported     = (delta_accum > 65535) ? 65535 : (uint16_t)delta_accum;
                delta_accum        = 0;
                delta_window_start = timer_read();
            }
        } else {
            if (delta >= POINTING_DEVICE_AS5600_TICK_COUNT) {
                uint8_t abs_d_mac = (uint8_t)delta;
                delta_accum += abs_d_mac;
                int16_t mult   = get_mult_fp(abs_d_mac, caps);
                current_position = ra;
                mouse_report.v = -(int8_t)(mult / 16);
            } else if (delta <= -POINTING_DEVICE_AS5600_TICK_COUNT) {
                uint8_t abs_d_mac = (uint8_t)(-delta);
                delta_accum += abs_d_mac;
                int16_t mult   = get_mult_fp(abs_d_mac, caps);
                current_position = ra;
                mouse_report.v = (int8_t)(mult / 16);
            }
        }
    }

    return mouse_report;
}

bool pointing_device_driver_init(void) { return true; }

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {{{ KC_NO }}};
