/* SPDX-License-Identifier: MIT */

#include <toucan/runtime_scaler.h>
#include <toucan/studio_rpc.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_DECLARE(toucan_studio, CONFIG_ZMK_LOG_LEVEL);

#define HAS_ACCEL (DT_NODE_HAS_STATUS(DT_NODELABEL(pointer_accel), okay) && \
                   IS_ENABLED(CONFIG_ZMK_INPUT_PROCESSOR_ACCELERATION))

#if HAS_ACCEL
#include <drivers/input_processor_accel.h>
static const struct device *const accel = DEVICE_DT_GET(DT_NODELABEL(pointer_accel));
#endif

#if IS_ENABLED(CONFIG_TOUCAN_INPUT_PROCESSOR_RUNTIME_SCALER)
static const struct device *const xy_scale = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(tcn_xy_scale));
static const struct device *const scroll_scale =
    DEVICE_DT_GET_OR_NULL(DT_NODELABEL(tcn_scroll_scale));
#endif

#define STANDARD_FIELDS(X)                                                                         \
    X(sensitivity, 300, 1000)                                                                      \
    X(min_factor, 200, 1500)                                                                       \
    X(max_factor, 1000, 3000)                                                                      \
    X(speed_threshold, 100, 2000)                                                                 \
    X(speed_max, 1000, 8000)
#define BOOST_FIELDS(X) X(y_boost, 1000, 3000)
#define SMOOTH_FIELDS(X)                                                                           \
    X(low_gain, 200, 2000)                                                                         \
    X(high_gain, 1000, 6000)                                                                       \
    X(mid_speed, 1, 100)                                                                          \
    X(ramp_width, 1, 100)                                                                         \
    X(smoothing, 1, 16)
#define XY_FIELDS(X) X(xy_mul, 1, 8) X(xy_div, 1, 8)
#define SCROLL_FIELDS(X) X(scroll_mul, 1, 4) X(scroll_div, 1, 16)
#define Y_BOOST_STEP 10
#define POINTER_RECORD_VERSION 2
#define POINTER_RECORD_V1_SIZE 22

struct pointer_record {
    uint32_t version;
    toucan_studio_PointerParams params;
};

static toucan_studio_PointerInfo caps;
static toucan_studio_PointerCurveInfo curve_caps;
static toucan_studio_PointerParams defaults;
static toucan_studio_PointerParams saved;

static toucan_studio_PointerParams read_current(void) {
    toucan_studio_PointerParams params = {0};
#if HAS_ACCEL
    struct accel_params live;
    if (caps.accel && accel_get_params(accel, &live) == 0) {
#define COPY(field, min, max) params.field = live.field;
        STANDARD_FIELDS(COPY)
        SMOOTH_FIELDS(COPY)
        BOOST_FIELDS(COPY)
        params.curve = live.curve;
#undef COPY
    }
#endif
#if IS_ENABLED(CONFIG_TOUCAN_INPUT_PROCESSOR_RUNTIME_SCALER)
    if (caps.xy_scale) {
        struct tcn_scaler_ratio ratio = tcn_scaler_get_ratio(xy_scale);
        params.xy_mul = ratio.mul;
        params.xy_div = ratio.div;
    }
    if (caps.scroll_scale) {
        struct tcn_scaler_ratio ratio = tcn_scaler_get_ratio(scroll_scale);
        params.scroll_mul = ratio.mul;
        params.scroll_div = ratio.div;
    }
#endif
    return params;
}

static int pointer_init(void) {
#if HAS_ACCEL
    struct accel_params live;
    caps.accel = accel_get_params(accel, &live) == 0;
#endif
#if IS_ENABLED(CONFIG_TOUCAN_INPUT_PROCESSOR_RUNTIME_SCALER)
    caps.xy_scale = device_is_ready(xy_scale);
    caps.scroll_scale = device_is_ready(scroll_scale);
#endif
#define RANGE(field, lo, hi)                                                                      \
    caps.has_##field = true;                                                                      \
    caps.field = (toucan_studio_Range){.min = lo, .max = hi};
    if (caps.accel) {
        STANDARD_FIELDS(RANGE)
        BOOST_FIELDS(RANGE)
        caps.y_boost_step = Y_BOOST_STEP;
    }
    if (caps.xy_scale) {
        XY_FIELDS(RANGE)
    }
    if (caps.scroll_scale) {
        SCROLL_FIELDS(RANGE)
    }
#undef RANGE
#define CURVE_RANGE(field, lo, hi)                                                               \
    curve_caps.has_##field = true;                                                               \
    curve_caps.field = (toucan_studio_Range){.min = lo, .max = hi};
    if (caps.accel) {
        curve_caps.curves = BIT(toucan_studio_PointerCurve_POINTER_CURVE_STANDARD) |
                            BIT(toucan_studio_PointerCurve_POINTER_CURVE_SMOOTH);
        SMOOTH_FIELDS(CURVE_RANGE)
    }
#undef CURVE_RANGE
    defaults = read_current();
    saved = defaults;
    return 0;
}

SYS_INIT(pointer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static uint8_t validate(const toucan_studio_PointerParams *p, bool all_curves) {
#define CHECK(field, lo, hi)                                                                      \
    if (!IN_RANGE(p->field, lo, hi)) {                                                             \
        return toucan_studio_PointerParams_##field##_tag;                                         \
    }
    if (caps.accel) {
        if ((uint32_t)p->curve > toucan_studio_PointerCurve_POINTER_CURVE_SMOOTH) {
            return toucan_studio_PointerParams_curve_tag;
        }
        BOOST_FIELDS(CHECK)
        if (all_curves || p->curve == toucan_studio_PointerCurve_POINTER_CURVE_STANDARD) {
            STANDARD_FIELDS(CHECK)
            if (p->min_factor > p->max_factor) {
                return toucan_studio_PointerParams_min_factor_tag;
            }
            if (p->speed_threshold >= p->speed_max) {
                return toucan_studio_PointerParams_speed_threshold_tag;
            }
        }
        if (all_curves || p->curve == toucan_studio_PointerCurve_POINTER_CURVE_SMOOTH) {
            SMOOTH_FIELDS(CHECK)
            if (p->high_gain < p->low_gain) {
                return toucan_studio_PointerParams_high_gain_tag;
            }
            if (p->ramp_width > p->mid_speed) {
                return toucan_studio_PointerParams_ramp_width_tag;
            }
        }
        if (p->y_boost % Y_BOOST_STEP) {
            return toucan_studio_PointerParams_y_boost_tag;
        }
    }
    if (caps.xy_scale) {
        XY_FIELDS(CHECK)
    }
    if (caps.scroll_scale) {
        SCROLL_FIELDS(CHECK)
    }
#undef CHECK
    return 0;
}

/* The caller validates the whole request before any device is changed. */
static int apply(const toucan_studio_PointerParams *p) {
#if HAS_ACCEL
    if (caps.accel) {
        const struct accel_params params = {
#define COPY(field, min, max) .field = p->field,
            STANDARD_FIELDS(COPY)
            SMOOTH_FIELDS(COPY)
            BOOST_FIELDS(COPY)
            .curve = p->curve,
#undef COPY
        };
        int err = accel_set_params(accel, &params);
        if (err) {
            return err;
        }
    }
#endif
#if IS_ENABLED(CONFIG_TOUCAN_INPUT_PROCESSOR_RUNTIME_SCALER)
    if (caps.xy_scale) {
        tcn_scaler_set_ratio(xy_scale, (struct tcn_scaler_ratio){p->xy_mul, p->xy_div});
    }
    if (caps.scroll_scale) {
        tcn_scaler_set_ratio(scroll_scale,
                             (struct tcn_scaler_ratio){p->scroll_mul, p->scroll_div});
    }
#endif
    return 0;
}

static uint8_t activity(void) {
    uint8_t bits = 0;
#if IS_ENABLED(CONFIG_TOUCAN_INPUT_PROCESSOR_RUNTIME_SCALER)
    if (caps.xy_scale && tcn_scaler_active(xy_scale)) {
        bits |= toucan_studio_PointerActivity_POINTER_ACTIVITY_CURSOR_CHAIN;
    }
    if (caps.scroll_scale && tcn_scaler_active(scroll_scale)) {
        bits |= toucan_studio_PointerActivity_POINTER_ACTIVITY_SCROLL_CHAIN;
    }
#endif
    return bits;
}

void tcn_pointer_info(toucan_studio_PointerInfo *info) { *info = caps; }

void tcn_pointer_curve_info(toucan_studio_Response *resp) {
    resp->which_result = toucan_studio_Response_pointer_curve_info_tag;
    resp->result.pointer_curve_info = curve_caps;
}

static bool params_equal(const toucan_studio_PointerParams *a,
                         const toucan_studio_PointerParams *b) {
#define EQUAL(field, lo, hi) a->field == b->field &&
    return STANDARD_FIELDS(EQUAL) SMOOTH_FIELDS(EQUAL) BOOST_FIELDS(EQUAL)
           XY_FIELDS(EQUAL) SCROLL_FIELDS(EQUAL) a->curve == b->curve;
#undef EQUAL
}

void tcn_pointer_get(toucan_studio_Response *resp) {
    toucan_studio_PointerParams current = read_current();
    resp->which_result = toucan_studio_Response_pointer_tag;
    resp->result.pointer = (toucan_studio_Pointer){
        .has_current = true,
        .current = current,
        .has_saved = true,
        .saved = saved,
        .unsaved = !params_equal(&current, &saved),
        .activity = activity(),
    };
}

void tcn_pointer_set(const toucan_studio_SetPointerRequest *req, toucan_studio_Response *resp) {
    if (!req->has_params) {
        tcn_rpc_error(resp, tcn_rpc_errno_to_code(-EINVAL), 0, 0);
        return;
    }
    if (!caps.accel && !caps.xy_scale && !caps.scroll_scale) {
        tcn_rpc_error(resp, tcn_rpc_errno_to_code(-ENOTSUP), 0, 0);
        return;
    }
    uint8_t field = validate(&req->params, false);
    if (field) {
        tcn_rpc_error(resp, tcn_rpc_errno_to_code(-ERANGE), field, 0);
        return;
    }
    toucan_studio_PointerParams merged = req->params;
    if (caps.accel) {
        const toucan_studio_PointerParams current = read_current();
#define KEEP(field, lo, hi) merged.field = current.field;
        if (merged.curve == toucan_studio_PointerCurve_POINTER_CURVE_STANDARD) {
            SMOOTH_FIELDS(KEEP)
        } else {
            STANDARD_FIELDS(KEEP)
        }
#undef KEEP
    }
    int err = apply(&merged);
    if (err) {
        tcn_rpc_error(resp, tcn_rpc_errno_to_code(err), 0, 0);
        return;
    }
    tcn_pointer_get(resp);
}

bool tcn_pointer_unsaved(void) {
    toucan_studio_PointerParams current = read_current();
    return !params_equal(&current, &saved);
}

int tcn_pointer_save(void) {
    struct pointer_record record = {
        .version = POINTER_RECORD_VERSION,
        .params = read_current(),
    };
    if (params_equal(&record.params, &saved)) {
        return 0;
    }
    int err = settings_save_one(TOUCAN_SETTINGS_POINTER, &record, sizeof(record));
    if (!err) {
        saved = record.params;
    }
    return err;
}

int tcn_pointer_discard(void) { return apply(&saved); }

int tcn_pointer_restore_defaults(void) {
    int err = settings_delete(TOUCAN_SETTINGS_POINTER);
    if (!err) {
        err = apply(&defaults);
        if (!err) {
            saved = defaults;
        }
    }
    return err;
}

static int load_pointer(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (name && *name) {
        return -ENOENT;
    }
    if (len == POINTER_RECORD_V1_SIZE) {
        uint16_t version;
        if (read_cb(cb_arg, &version, sizeof(version)) == sizeof(version) && version == 1) {
            LOG_WRN("Ignoring pointer settings record version 1");
            return 0;
        }
    }
    struct pointer_record record;
    if (len != sizeof(record) || read_cb(cb_arg, &record, sizeof(record)) != sizeof(record) ||
        record.version != POINTER_RECORD_VERSION) {
        LOG_WRN("Ignoring malformed or unsupported pointer settings record");
        return 0;
    }
    uint8_t field = validate(&record.params, true);
    if (field) {
        LOG_WRN("Ignoring pointer settings: invalid field %u", field);
        return 0;
    }
    int err = apply(&record.params);
    if (err) {
        LOG_WRN("Ignoring pointer settings: apply failed (%d)", err);
        return 0;
    }
    saved = read_current();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tcn_ptr, TOUCAN_SETTINGS_POINTER, NULL, load_pointer, NULL, NULL);
