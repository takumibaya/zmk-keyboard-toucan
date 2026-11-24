/*
 * Touch gestures of the left half (src/touch_gestures.c): parameters, the on/off key state
 * and the diagnostics that the Studio RPC (src/gesture_rpc.c) reads and replaces.
 *
 * Parameters and the key state may change from any thread; the input thread picks the change
 * up with the next trackpad event, and a touch in progress then stops gesturing.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Settings key of the on/off key state (absent = on). */
#define TOUCAN_SETTINGS_GESTURE_SWITCH "tcn/gsw"

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

/* The keymap DT values (also the result of restore_defaults). */
void tcn_gestures_defaults(struct tcn_gesture_params *params);

/* The parameters in effect. */
void tcn_gestures_get(struct tcn_gesture_params *params);

/* Replace the parameters in effect; the caller has validated them. */
void tcn_gestures_set(const struct tcn_gesture_params *params);

/* Rim scroll is built (aux coordinates and a wheel output are configured). */
bool tcn_gestures_rim_available(void);

/* The latest touch (current or last) carried aux coordinates from the right half. */
bool tcn_gestures_rim_samples_seen(void);

/* The on/off key has turned the gestures off. */
bool tcn_gestures_switched_off(void);

/* On/off key: TGS_TOG / TGS_ON / TGS_OFF (dt-bindings/toucan/touch_gestures.h). The state is
 * stored right away so it survives a reboot. Returns -EINVAL for an unknown op. */
int tcn_gestures_switch(uint32_t op);

/* Turn the key state back on and erase it from the settings (restore_defaults, official reset). */
int tcn_gestures_switch_reset(void);
