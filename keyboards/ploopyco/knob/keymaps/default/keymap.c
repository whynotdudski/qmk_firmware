/* Copyright 2023 Colin Lam (Ploopy Corporation)
 * Eden's custom firmware — delta-based acceleration
 *
 * HOW IT WORKS:
 *   SPEED_DIV=2, DEADZONE=12. Original: v = -delta/2 = 6 lines per tick.
 *   This firmware: accumulates scroll into a int32 buffer, flushes as
 *   multiple int8 reports per poll so high multipliers actually work.
 *   Multiplier is ×16 fixed-point stored as uint16: 16=1×, 32=2×, 480=30×.
 *   Curve X = accumulated abs(delta) over 50ms window (65–900+ real range).
 *
 * HID COMMANDS:
 *   0x01  = toggle Ableton layer
 *   0x02  = force normal layer
 *   0x03  = force Ableton layer
 *   0x10  = update curve  [cmd, count, d_lo, d_hi, slow_lo, slow_hi, fast_lo, fast_hi, ...]
 *   0x11  = query delta   returns [0x11, accum_lo, accum_hi]
 */

#include QMK_KEYBOARD_H
#include "as5600.h"

#ifdef RAW_ENABLE
#include "raw_hid.h"
#endif

enum layers { _SCROLL = 0, _ABLETON };

// ─── Curve ────────────────────────────────────────────────────────────────────
#define MAX_CURVE_POINTS 8

// X thresholds: accumulated abs(delta) per 50ms window
// Real ranges: slow 65-195, medium 200-450, fast 450-700, max 700-950
static uint16_t curve_deltas[MAX_CURVE_POINTS] = {
     65,   // slow deliberate
    200,   // medium
    400,   // fast
    650,   // very fast
    900,   // max spin
      0, 0, 0
};

// Multipliers ×16 fixed-point stored as uint16
// 16=1×  32=2×  48=3×  160=10×  480=30×  800=50×
static uint16_t slow_mults[MAX_CURVE_POINTS] = {
     16,   // 1.0× — identical to original
     24,   // 1.5×
     64,   // 4×
    160,   // 10×
    320,   // 20×
    320, 320, 320
};

static uint16_t fast_mults[MAX_CURVE_POINTS] = {
     32,   // 2×
     64,   // 4×
    160,   // 10×
    320,   // 20×
    640,   // 40×
    640, 640, 640
};

static uint8_t  curve_point_count = 5;

// ─── State ────────────────────────────────────────────────────────────────────
static uint16_t current_position   = 0;
static bool     dragging           = false;
static uint16_t last_move_time     = 0;
static int32_t  scroll_accum       = 0;   // fractional scroll buffer
static uint32_t delta_accum        = 0;   // speed measurement accumulator
static uint16_t delta_window_start = 0;
static uint16_t delta_reported     = 0;   // last 50ms window sum, for GUI

#define DRAG_RELEASE_TIMEOUT 400
#define SCROLL_SCALE         16           // fixed-point denominator

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

        // Packet: [0x10, count, d_lo, d_hi, slow_lo, slow_hi, fast_lo, fast_hi, ...]
        // Each point = 6 bytes of payload (2 for delta, 2 for slow, 2 for fast)
        case 0x10: {
            uint8_t count = data[1];
            if (count < 2 || count > MAX_CURVE_POINTS) break;
            for (uint8_t i = 0; i < count; i++) {
                uint8_t o = 2 + i * 6;
                if (o + 5 >= length) break;
                curve_deltas[i] = data[o]   | ((uint16_t)data[o+1] << 8);
                slow_mults[i]   = data[o+2] | ((uint16_t)data[o+3] << 8);
                fast_mults[i]   = data[o+4] | ((uint16_t)data[o+5] << 8);
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

// ─── Multiplier lookup ────────────────────────────────────────────────────────
// Returns multiplier ×16. Pass delta_reported (50ms accumulation).
static int32_t get_mult_fp(uint16_t spd, bool caps) {
    uint16_t *mults = caps ? fast_mults : slow_mults;

    if (spd <= curve_deltas[0])
        return (int32_t)mults[0];

    for (uint8_t i = 0; i < curve_point_count - 1; i++) {
        uint16_t d_lo = curve_deltas[i];
        uint16_t d_hi = curve_deltas[i+1];
        if (d_hi == 0) break;
        if (spd >= d_lo && spd < d_hi) {
            // Linear interpolation
            int32_t frac16 = ((int32_t)(spd - d_lo) * 16) / (d_hi - d_lo);
            int32_t m_lo   = (int32_t)mults[i];
            int32_t m_hi   = (int32_t)mults[i+1];
            return m_lo + (frac16 * (m_hi - m_lo)) / 16;
        }
    }

    return (int32_t)mults[curve_point_count - 1];
}

// ─── Sensor task ──────────────────────────────────────────────────────────────
report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    uint16_t ra    = as5600_get_rawangle();
    int16_t  delta = (int16_t)(ra - current_position);

    if (delta > 2048)       delta -= 4096;
    else if (delta < -2048) delta += 4096;

    bool caps = host_keyboard_led_state().caps_lock;

    // ── Speed window snapshot ─────────────────────────────────────────────────
    if (timer_elapsed(delta_window_start) >= 50) {
        delta_reported     = (uint16_t)(delta_accum > 0xFFFF ? 0xFFFF : delta_accum);
        delta_accum        = 0;
        delta_window_start = timer_read();
    }

    if (IS_LAYER_ON(_ABLETON)) {
        if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
            uint16_t abs_d2 = (delta < 0 ? -delta : delta);
            delta_accum += abs_d2;
            int32_t mult = get_mult_fp((uint16_t)(abs_d2 * 50), caps);
            if (!dragging) { mouse_report.buttons |= MOUSE_BTN1; dragging = true; }
            if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
                mouse_report.y = (int8_t)((-delta * mult) / (POINTING_DEVICE_AS5600_SPEED_DIV * SCROLL_SCALE));
            } else {
                mouse_report.y = (delta > 0) ? -(int8_t)(mult/SCROLL_SCALE) : (int8_t)(mult/SCROLL_SCALE);
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
                uint16_t abs_d = (delta < 0 ? -delta : delta);
                delta_accum += abs_d;
                int32_t mult = get_mult_fp((uint16_t)(abs_d * 50), caps);
                scroll_accum += (-delta * mult) / POINTING_DEVICE_AS5600_SPEED_DIV;
                current_position = ra;
            }

            // Flush scroll accumulator: extract whole lines, leave remainder
            // This handles multipliers > 127 by sending multiple reports if needed
            int32_t lines = scroll_accum / SCROLL_SCALE;
            scroll_accum -= lines * SCROLL_SCALE;

            // Clamp to int8 range per report; remainder carries to next poll
            if      (lines >  127) { mouse_report.v =  127; scroll_accum += (lines - 127) * SCROLL_SCALE; }
            else if (lines < -127) { mouse_report.v = -127; scroll_accum += (lines + 127) * SCROLL_SCALE; }
            else                   { mouse_report.v = (int8_t)lines; }

        } else {
            // macOS tick-based
            if (delta >= POINTING_DEVICE_AS5600_TICK_COUNT) {
                delta_accum += (uint16_t)delta;
                int32_t mult = get_mult_fp((uint16_t)(delta * 50), caps);
                current_position = ra;
                mouse_report.v = -(int8_t)(mult / SCROLL_SCALE);
            } else if (delta <= -POINTING_DEVICE_AS5600_TICK_COUNT) {
                delta_accum += (uint16_t)(-delta);
                int32_t mult = get_mult_fp((uint16_t)((-delta) * 50), caps);
                current_position = ra;
                mouse_report.v = (int8_t)(mult / SCROLL_SCALE);
            }
        }
    }

    return mouse_report;
}

bool pointing_device_driver_init(void) { return true; }

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {{{ KC_NO }}};
