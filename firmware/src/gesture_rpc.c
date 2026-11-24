/*
 * Touch gesture settings over toucan__studio (TouchGesture* in proto/toucan/studio.proto).
 * The parameters live in src/touch_gestures.c; this file validates, stores and restores them.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zmk/behavior.h>

#include <toucan/studio_rpc.h>
#include <toucan/touch_gestures.h>

LOG_MODULE_DECLARE(toucan_studio, CONFIG_ZMK_LOG_LEVEL);

#define GESTURE_RECORD_VERSION 1

struct gesture_record {
    uint16_t version;
    struct tcn_gesture_params params;
};

BUILD_ASSERT(sizeof(struct gesture_record) == 12, "gesture record layout (version 1) changed");

static struct tcn_gesture_params saved;

static int gesture_rpc_init(void) {
    tcn_gestures_defaults(&saved);
    return 0;
}

SYS_INIT(gesture_rpc_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

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

/* A validated full set; the rim values in effect stay when rim scroll is not built. */
static struct tcn_gesture_params from_proto(const toucan_studio_TouchGestureParams *p) {
    struct tcn_gesture_params params;
    tcn_gestures_get(&params);
    params.tap = p->tap;
    params.tap_ms = p->tap_ms;
    params.tap_move = p->tap_move;
    if (tcn_gestures_rim_available()) {
        params.rim = p->rim;
        params.rim_width = p->rim_width;
        params.rim_start_deg = p->rim_start_deg;
        params.rim_steps = p->rim_steps;
        params.rim_reverse = p->rim_reverse;
    }
    return params;
}

static bool params_equal(const struct tcn_gesture_params *a, const struct tcn_gesture_params *b) {
    return a->tap == b->tap && a->tap_ms == b->tap_ms && a->tap_move == b->tap_move &&
           a->rim == b->rim && a->rim_width == b->rim_width &&
           a->rim_start_deg == b->rim_start_deg && a->rim_steps == b->rim_steps &&
           a->rim_reverse == b->rim_reverse;
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
    struct tcn_gesture_params current;
    tcn_gestures_get(&current);
    resp->which_result = toucan_studio_Response_touch_gesture_settings_tag;
    resp->result.touch_gesture_settings = (toucan_studio_TouchGestures){
        .has_current = true,
        .current = to_proto(&current),
        .has_saved = true,
        .saved = to_proto(&saved),
        .unsaved = !params_equal(&current, &saved),
        .switched_off = tcn_gestures_switched_off(),
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
    const struct tcn_gesture_params params = from_proto(&req->params);
    tcn_gestures_set(&params);
    tcn_gesture_get(resp);
}

bool tcn_gesture_unsaved(void) {
    struct tcn_gesture_params current;
    tcn_gestures_get(&current);
    return !params_equal(&current, &saved);
}

int tcn_gesture_save(void) {
    struct gesture_record record = {.version = GESTURE_RECORD_VERSION};
    tcn_gestures_get(&record.params);
    if (params_equal(&record.params, &saved)) {
        return 0;
    }
    int err = settings_save_one(TOUCAN_SETTINGS_GESTURE, &record, sizeof(record));
    if (!err) {
        saved = record.params;
    }
    return err;
}

int tcn_gesture_discard(void) {
    tcn_gestures_set(&saved);
    return 0;
}

int tcn_gesture_restore_defaults(void) {
    int err = settings_delete(TOUCAN_SETTINGS_GESTURE);
    if (err) {
        return err;
    }
    tcn_gestures_defaults(&saved);
    tcn_gestures_set(&saved);
    return tcn_gestures_switch_reset();
}

static int load_gesture(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (name && *name) {
        return -ENOENT;
    }
    struct gesture_record record;
    if (len != sizeof(record) || read_cb(cb_arg, &record, sizeof(record)) != sizeof(record) ||
        record.version != GESTURE_RECORD_VERSION) {
        LOG_WRN("Ignoring malformed or unsupported gesture settings record");
        return 0;
    }
    toucan_studio_TouchGestureParams params = to_proto(&record.params);
    if (!tcn_gestures_rim_available() && params.rim) {
        LOG_WRN("Stored rim scroll is not built here; loading the rest");
        params.rim = false;
    }
    toucan_studio_ErrorCode code;
    uint8_t field = validate(&params, &code);
    if (field) {
        LOG_WRN("Ignoring gesture settings: invalid field %u", field);
        return 0;
    }
    saved = from_proto(&params);
    tcn_gestures_set(&saved);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tcn_gst, TOUCAN_SETTINGS_GESTURE, NULL, load_gesture, NULL, NULL);
