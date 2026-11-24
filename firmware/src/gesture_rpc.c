/*
 * Touch gesture settings over toucan__studio (TouchGesture* in proto/toucan/studio.proto).
 * The parameters and their stored copy live in src/touch_gestures.c (the &tgs key changes them
 * too); this file validates requests and converts between the module and the protocol.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/logging/log.h>
#include <zmk/behavior.h>

#include <toucan/studio_rpc.h>
#include <toucan/touch_gestures.h>

LOG_MODULE_DECLARE(toucan_studio, CONFIG_ZMK_LOG_LEVEL);

static bool in_range(uint32_t value, uint32_t min, uint32_t max) {
    return value >= min && value <= max;
}

/* 0, or the TouchGestureParams field number of the first value that is rejected with *code. */
static uint8_t validate(const toucan_studio_TouchGestureParams *p, toucan_studio_ErrorCode *code) {
    *code = toucan_studio_ErrorCode_ERROR_CODE_OUT_OF_RANGE;
#define CHECK(field, lo, hi)                                                                       \
    if (!in_range(p->field, lo, hi)) {                                                             \
        return toucan_studio_TouchGestureParams_##field##_tag;                                     \
    }
    TCN_GESTURE_TAP_RANGES(CHECK)
    if (tcn_gestures_rim_available()) {
        TCN_GESTURE_RIM_RANGES(CHECK)
    } else if (p->rim) {
        *code = toucan_studio_ErrorCode_ERROR_CODE_NOT_SUPPORTED;
        return toucan_studio_TouchGestureParams_rim_tag;
    }
#undef CHECK
    return 0;
}

static toucan_studio_TouchGestureParams to_proto(const struct tcn_gesture_params *p) {
    return (toucan_studio_TouchGestureParams){
        .tap = p->tap,
        .tap_ms = p->tap_ms,
        .tap_move = p->tap_move,
        .rim = p->rim,
        .rim_width = p->rim_width,
        .rim_start_deg = p->rim_start_deg,
        .rim_steps = p->rim_steps,
        .rim_reverse = p->rim_reverse,
    };
}

void tcn_gesture_info(toucan_studio_Response *resp) {
    resp->which_result = toucan_studio_Response_touch_gesture_info_tag;
    toucan_studio_TouchGestureInfo *info = &resp->result.touch_gesture_info;
    *info = (toucan_studio_TouchGestureInfo)toucan_studio_TouchGestureInfo_init_zero;
#define RANGE(field, lo, hi)                                                                       \
    info->has_##field = true;                                                                      \
    info->field = (toucan_studio_Range){.min = lo, .max = hi};
    info->tap = true;
    TCN_GESTURE_TAP_RANGES(RANGE)
    if (tcn_gestures_rim_available()) {
        info->rim = true;
        TCN_GESTURE_RIM_RANGES(RANGE)
    }
#undef RANGE
    info->switch_behavior_id =
        zmk_behavior_get_local_id(DEVICE_DT_GET(DT_INST(0, toucan_behavior_gesture_switch))->name);
}

void tcn_gesture_get(toucan_studio_Response *resp) {
    struct tcn_gesture_params current, saved;
    tcn_gestures_get(&current, &saved);
    resp->which_result = toucan_studio_Response_touch_gesture_settings_tag;
    resp->result.touch_gesture_settings = (toucan_studio_TouchGestures){
        .has_current = true,
        .current = to_proto(&current),
        .has_saved = true,
        .saved = to_proto(&saved),
        .unsaved = !tcn_gestures_params_equal(&current, &saved),
        .rim_samples_seen = tcn_gestures_rim_samples_seen(),
    };
}

void tcn_gesture_set(const toucan_studio_SetTouchGestureRequest *req,
                     toucan_studio_Response *resp) {
    if (!req->has_params) {
        tcn_rpc_error(resp, tcn_rpc_errno_to_code(-EINVAL), 0, 0);
        return;
    }
    toucan_studio_ErrorCode code;
    uint8_t field = validate(&req->params, &code);
    if (field) {
        tcn_rpc_error(resp, code, field, 0);
        return;
    }
    /* Full replace; without rim support the rim values in effect stay. */
    struct tcn_gesture_params params, saved;
    tcn_gestures_get(&params, &saved);
    params.tap = req->params.tap;
    params.tap_ms = req->params.tap_ms;
    params.tap_move = req->params.tap_move;
    if (tcn_gestures_rim_available()) {
        params.rim = req->params.rim;
        params.rim_width = req->params.rim_width;
        params.rim_start_deg = req->params.rim_start_deg;
        params.rim_steps = req->params.rim_steps;
        params.rim_reverse = req->params.rim_reverse;
    }
    tcn_gestures_set(&params);
    tcn_gesture_get(resp);
}

bool tcn_gesture_unsaved(void) {
    struct tcn_gesture_params current, saved;
    tcn_gestures_get(&current, &saved);
    return !tcn_gestures_params_equal(&current, &saved);
}

int tcn_gesture_save(void) { return tcn_gestures_save(); }

int tcn_gesture_discard(void) {
    tcn_gestures_discard();
    return 0;
}

int tcn_gesture_restore_defaults(void) { return tcn_gestures_restore_defaults(); }
