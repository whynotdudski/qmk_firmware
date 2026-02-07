/* Copyright 2023 Colin Lam (Ploopy Corporation)
 * Modified for dual-layer with adjustable velocity-based acceleration
 * Configuration: adapted_1
 */

#include QMK_KEYBOARD_H
#include "as5600.h"

#ifdef RAW_ENABLE
#include "raw_hid.h"
#endif

// ===== ACCELERATION SETTINGS - ADJUST THESE VALUES =====
// Time thresholds in milliseconds (lower = faster rotation needed)
#define ACCEL_THRESHOLD_VFAST 20   // Very fast spinning
#define ACCEL_THRESHOLD_FAST  40   // Fast rotation
#define ACCEL_THRESHOLD_MED   80   // Medium rotation
#define ACCEL_THRESHOLD_SLOW  150  // Gentle rotation

// Speed multipliers for each threshold
#define ACCEL_MULT_VFAST 5  // 5x speed for very fast
#define ACCEL_MULT_FAST  3  // 3x speed for fast
#define ACCEL_MULT_MED   2  // 2x speed for medium
#define ACCEL_MULT_SLOW  1  // 1x speed (normal, no acceleration)
// =======================================================

// Layers
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
        case 0x01:  // Toggle Ableton layer
            layer_invert(_ABLETON);
            break;
        case 0x02:  // Force scroll layer
            layer_off(_ABLETON);
            break;
        case 0x03:  // Force Ableton layer
            layer_on(_ABLETON);
            break;
    }
}
#endif

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

    // Calculate velocity-based acceleration
    uint16_t time_since_last = timer_elapsed(last_rotation_time);
    int16_t speed_multiplier;
    
    if (time_since_last < ACCEL_THRESHOLD_VFAST) {
        speed_multiplier = ACCEL_MULT_VFAST;
    } else if (time_since_last < ACCEL_THRESHOLD_FAST) {
        speed_multiplier = ACCEL_MULT_FAST;
    } else if (time_since_last < ACCEL_THRESHOLD_MED) {
        speed_multiplier = ACCEL_MULT_MED;
    } else if (time_since_last < ACCEL_THRESHOLD_SLOW) {
        speed_multiplier = ACCEL_MULT_SLOW;
    } else {
        speed_multiplier = 1;  // Very slow = no acceleration
    }

    if (IS_LAYER_ON(_ABLETON)) {
        // ABLETON LAYER: Click + drag with acceleration
        if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
            if (!dragging) {
                mouse_report.buttons |= MOUSE_BTN1;  // Left click down
                dragging = true;
            }
            
            // Move mouse vertically with acceleration (inverted)
            if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
                mouse_report.y = (-delta * speed_multiplier) / POINTING_DEVICE_AS5600_SPEED_DIV;
            } else {
                mouse_report.y = (delta > 0) ? -speed_multiplier : speed_multiplier;
            }
            
            current_position = ra;
            last_rotation_time = now;
        }
        
        // Release click after 150ms of no rotation
        if (dragging && timer_elapsed(last_rotation_time) > 150) {
            mouse_report.buttons &= ~MOUSE_BTN1;  // Left click up
            dragging = false;
        }
        
    } else {
        // SCROLL LAYER: Normal scroll with acceleration (inverted)
        if (detected_host_os() == OS_WINDOWS || detected_host_os() == OS_LINUX) {
            if (delta > POINTING_DEVICE_AS5600_DEADZONE || delta < -POINTING_DEVICE_AS5600_DEADZONE) {
                current_position = ra;
                mouse_report.v = (-delta * speed_multiplier) / POINTING_DEVICE_AS5600_SPEED_DIV;
            }
        } else {
            if (delta >= POINTING_DEVICE_AS5600_TICK_COUNT) {
                current_position = ra;
                mouse_report.v = -speed_multiplier;  // Inverted with acceleration
            } else if (delta <= -POINTING_DEVICE_AS5600_TICK_COUNT) {
                current_position = ra;
                mouse_report.v = speed_multiplier;  // Inverted with acceleration
            }
        }
    }

    return mouse_report;
}

bool pointing_device_driver_init(void) {
    return true;
}
