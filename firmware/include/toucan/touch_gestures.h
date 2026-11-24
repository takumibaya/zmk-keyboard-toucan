/*
 * Touch gestures of the left half (src/touch_gestures.c): parameters, their stored copy and
 * the diagnostics that the Studio RPC (src/gesture_rpc.c) and the on/off key (&tgs) use.
 *
 * Parameters may change from any thread; the input thread picks a change up with the next
 * trackpad event, and a touch in progress then stops gesturing.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Settings key of the stored parameters (one versioned record). */
#define TOUCAN_SETTINGS_GESTURE "tcn/gst"

/* name, min, max of the numeric parameters (TouchGestureParams; also checked for DT defaults). */
#define TCN_GESTURE_TAP_RANGES(X) X(tap_ms, 60, 350) X(tap_move, 0, 255)
#define TCN_GESTURE_RIM_RANGES(X) X(rim_width, 5, 40) X(rim_start_deg, 5, 90) X(rim_steps, 1, 32)

/* One complete set of gesture parameters (TouchGestureParams in studio.proto). */
struct tcn_gesture_params {
    uint16_t tap_ms;       /* longest BTN_TOUCH press-to-release that clicks */
    uint16_t tap_move;     /* most accumulated |dx|+|dy| before acceleration that clicks */
    uint8_t rim_width;     /* rim band, % of the pad radius */
    uint8_t rim_start_deg; /* angle in one direction before the rim scrolls */
    uint8_t rim_steps;     /* wheel steps per 90 degrees */
    bool tap;
    bool rim;
    bool rim_reverse; /* false: clockwise scrolls down */
};

bool tcn_gestures_params_equal(const struct tcn_gesture_params *a,
                               const struct tcn_gesture_params *b);

/* The keymap DT values (also the result of restore_defaults). */
void tcn_gestures_defaults(struct tcn_gesture_params *params);

/* The parameters in effect and the stored ones (DT values while nothing is stored). */
void tcn_gestures_get(struct tcn_gesture_params *current, struct tcn_gesture_params *saved);

/* Replace the parameters in effect (RAM only); the caller has validated them. */
void tcn_gestures_set(const struct tcn_gesture_params *params);

/* Store the parameters in effect. */
int tcn_gestures_save(void);

/* Put the stored parameters back in effect. */
void tcn_gestures_discard(void);

/* DT values in effect and stored (the record is erased). */
int tcn_gestures_restore_defaults(void);

/* On/off key: TGS_ON turns every built gesture on, TGS_OFF turns all off, TGS_TOG turns all off
 * when any is on and on otherwise (dt-bindings/toucan/touch_gestures.h). Only the on/off flags
 * change, in effect and stored (written right away); the other parameters and any other unsaved
 * change stay as they are. Returns -EINVAL for an unknown op or the settings error. */
int tcn_gestures_switch(uint32_t op);

/* Rim scroll is built (aux coordinates and a wheel output are configured). */
bool tcn_gestures_rim_available(void);

/* The latest touch (current or last) carried aux coordinates from the right half. */
bool tcn_gestures_rim_samples_seen(void);
