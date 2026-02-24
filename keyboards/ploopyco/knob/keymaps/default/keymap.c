/* Copyright 2023 Colin Lam (Ploopy Corporation)
 * Eden's custom firmware — precision-first acceleration
 *
 * ACCELERATION CURVE:
 *   - Stored in RAM as arrays — no reflash needed to update via GUI
 *   - Catmull-Rom style linear interpolation between control points
 *   - Send new curve via Raw HID from knob_curve_editor.py
 *   - To make permanent: paste the exported #defines here and rebuild
 *
 * HID COMMANDS:
 *   0x01  = toggle Ableton layer
 *   0x02  = force scroll layer
 *   0x03  = force Ableton layer
 *   0x10  = update curve (packet: [0x10, count, ms_lo, ms_hi, slow, fast, ...])
 *
 * TUNING WITHOUT GUI:
 *   Edit the DEFAULT_THRESHOLDS / DEFAULT_SLOW / DEFAULT_FAST arrays below.
 *   Values are sorted ascending by speed (fastest tier first = lowest ms).
 */

#include QMK_KEYBOARD_H
#include "as5600.h"

#ifdef RAW_ENABLE
#include "raw_hid.h"
#endif

// ─── Layers ──────────────────────────────────────────────────────────────────
enum layers {
    _SCROLL = 0,
    _ABLETON
};

// ─── Acceleration curve — stored in RAM ──────────────────────────────────────
// Each index is a "tier". Tiers are sorted by threshold ASCENDING
// (index 0 = fastest spin / shortest time between readings).
// The firmware walks the array and uses the first tier where
// time_since_last < threshold[i].
// If time exceeds ALL thresholds → uses the last (baseline) tier.

#define MAX_CURVE_POINTS 8

// Default curve — edit these to change baked-in defaults
// Format: { fastest_threshold, ..., slowest_threshold }
// Threshold = ms between sensor readings. Lower = only triggers at high speed.
static uint16_t curve_thresholds[MAX_CURVE_POINTS] = {
    12,   // tier 0: ultrafast  (spinning very hard)
    25,   // tier 1: very fast
    50,   // tier 2: fast
    100,  // tier 3: moderate
    200,  // tier 4: deliberate pickup
    350,  // tier 5: gentle pickup
    0,    // unused
    0     // unused
};

// SLOW mode (CapsLock OFF) — multipliers for each tier
// Index matches curve_thresholds above
static uint8_t slow_mults[MAX_CURVE_POINTS] = {
    18,   // ultrafast  → 18 lines
    12,   // very fast  → 12 lines
     7,   // fast       → 7 lines
     4,   // moderate   → 4 lines
     2,   // deliberate → 2 lines
     1,   // gentle     → 1 line (this is still "picking up")
     1,   // unused (baseline — applies when ALL thresholds exceeded)
     1    // unused
};

// FAST mode (CapsLock ON) — much higher multipliers
static uint8_t fast_mults[MAX_CURVE_POINTS] = {
    50,   // ultrafast
    35,   // very fast
    20,   // fast
    12,   // moderate
     6,   // deliberate
     3,   // gentle
     3,   // baseline in fast mode (even slow = 3×)
     3    // unused
};

// Actual count of active tiers (updated by HID receive)
static uint8_t curve_point_count = 6;

// ─── Ableton layer ────────────────────────────────────────────────────────────
#define DRAG_RELEASE_TIMEOUT 400
static uint16_t current_position   = 0;
static bool     dragging           = false;
static uint16_t last_rotation_time = 0;

// ─── Init ─────────────────────────────────────────────────────────────────────
void keyboard_post_init_user(void) {
    as5600_init();
    current_position = as5600_get_rawangle();
}

// ─── HID receive ─────────────────────────────────────────────────────────────
#ifdef RAW_ENABLE
void raw_hid_receive(uint8_t *data, uint8_t length) {
    // On Windows via hidapi, hid.write() first byte is the report ID.
    // QMK's raw_hid_receive gets the packet AFTER report ID is stripped.
    // So data[0] = command byte (what Python sends as pkt[1]).
    // Python packet: [0x00(report_id), cmd, payload...]
    // Firmware sees: [cmd, payload...]
    uint8_t cmd = data[0];

    switch (cmd) {

        // Layer control (matches existing toggle_knob_layer.py behavior)
        case 0x01: layer_invert(_ABLETON); break;
        case 0x02: layer_off(_ABLETON);    break;
        case 0x03: layer_on(_ABLETON);     break;

        // Speed query — GUI polls for live monitor dot
        // Returns time elapsed since last movement — large when idle, small when spinning
        case 0x11: {
            uint8_t resp[32] = {0};
            uint16_t t = timer_elapsed(last_rotation_time);
            resp[0] = 0x11;
            resp[1] = t & 0xFF;
            resp[2] = (t >> 8) & 0xFF;
            raw_hid_send(resp, 32);
            break;
        }

        // Curve update — data[0]=0x10, data[1]=count, data[2+]=points
        case 0x10: {
            uint8_t count = data[1];
            if (count < 2 || count > MAX_CURVE_POINTS) break;
            for (uint8_t i = 0; i < count; i++) {
                uint8_t offset = 2 + i * 4;
                if (offset + 3 >= length) break;
                uint16_t ms   = data[offset] | ((uint16_t)data[offset + 1] << 8);
                uint8_t  slow = data[offset + 2];
                uint8_t  fast = data[offset + 3];
                curve_thresholds[i] = ms;
                slow_mults[i]       = slow;
                fast_mults[i]       = fast;
            }
            for (uint8_t i = count; i < MAX_CURVE_POINTS; i++) {
                curve_thresholds[i] = 0;
                slow_mults[i]       = slow_mults[count - 1];
                fast_mults[i]       = fast_mults[count - 1];
            }
            curve_point_count = count;
            break;
        }
    }
}
#endif

// ─── Interpolated multiplier ──────────────────────────────────────────────────
// Linear interpolation between adjacent tiers for smooth transitions.
// Returns multiplier * 16 (fixed-point) — caller divides by 16.
static int16_t get_multiplier_fp(bool caps) {
    uint16_t t = timer_elapsed(last_rotation_time);

    uint8_t *mults = caps ? fast_mults : slow_mults;

    // Below the fastest threshold
    if (t < curve_thresholds[0]) {
        return (int16_t)mults[0] * 16;
    }

    // Find which two tiers we're between and interpolate
    for (uint8_t i = 0; i < curve_point_count - 1; i++) {
        uint16_t t_lo = curve_thresholds[i];     // faster tier
        uint16_t t_hi = curve_thresholds[i + 1]; // slower tier

        if (t >= t_lo && t < t_hi) {
            // How far between the two tiers (0..1 as fixed-point 0..16)
            uint16_t range   = t_hi - t_lo;
            uint16_t pos     = t - t_lo;
            uint16_t frac16  = (pos * 16) / range;  // 0..16

            int16_t m_fast = (int16_t)mults[i];
            int16_t m_slow = (int16_t)mults[i + 1];

            // Interpolate: at t_lo → m_fast, at t_hi → m_slow
            return m_fast * 16 + (int16_t)frac16 * (m_slow - m_fast);
        }
    }

    // Above all thresholds — baseline (last tier)
    return (int16_t)mults[curve_point_count - 1] * 16;
}

// ─── Main pointing device task ───────────────────────────────────────────────
report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    uint16_t ra    = as5600_get_rawangle();
    int16_t  delta = (int16_t)(ra - current_position);
    uint16_t now   = timer_read();

    if (delta > 2048)       delta -= 4096;
    else if (delta < -2048) delta += 4096;

    bool caps = host_keyboard_led_state().caps_lock;

    if (IS_LAYER_ON(_ABLETON)) {
        // ── Ableton click-drag mode ───────────────────────────────────────────
        if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
            int16_t spd_fp = get_multiplier_fp(caps);  // fixed-point × 16

            if (!dragging) {
                mouse_report.buttons |= MOUSE_BTN1;
                dragging = true;
            }
            // Inverted to match scroll layer direction (note the minus on delta)
            if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
                mouse_report.y = (int8_t)((-delta * spd_fp) / (POINTING_DEVICE_AS5600_SPEED_DIV * 16));
            } else {
                int16_t dir = (delta > 0) ? -1 : 1;
                mouse_report.y = (int8_t)(dir * spd_fp / 16);
            }
            current_position   = ra;
            last_rotation_time = now;
        }
        if (dragging && timer_elapsed(last_rotation_time) > DRAG_RELEASE_TIMEOUT) {
            mouse_report.buttons &= ~MOUSE_BTN1;
            dragging = false;
        }

    } else {
        // ── Scroll mode (default) ─────────────────────────────────────────────
        if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
            if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
                int16_t spd_fp = get_multiplier_fp(caps);  // fixed-point × 16
                current_position   = ra;
                last_rotation_time = now;
                // Divide by 16 to resolve fixed-point, divide by SPEED_DIV for hardware calibration
                mouse_report.v = (int8_t)((-delta * spd_fp) / (POINTING_DEVICE_AS5600_SPEED_DIV * 16));
            }
        } else {
            // macOS / other — tick-based
            if (delta >= POINTING_DEVICE_AS5600_TICK_COUNT) {
                current_position = ra;
                int16_t spd_fp = get_multiplier_fp(caps);
                mouse_report.v = -(int8_t)(spd_fp / 16);
            } else if (delta <= -POINTING_DEVICE_AS5600_TICK_COUNT) {
                current_position = ra;
                int16_t spd_fp = get_multiplier_fp(caps);
                mouse_report.v = (int8_t)(spd_fp / 16);
            }
        }
    }

    return mouse_report;
}

bool pointing_device_driver_init(void) {
    return true;
}

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {{{ KC_NO }}};
