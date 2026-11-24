/*
 * Touch gestures of the left half: tap-to-click and rim scroll on the right half's trackpad.
 *
 * The trackpad listener uses this node (toucan,input-gestures) as its `device`. Every event of
 * the source device (the right half's split proxy) is handled here on the input thread and
 * handed synchronously to the listeners of this node, without a second trip through the input
 * queue. While the gestures are off (the default) every event goes through unchanged. The
 * right half's aux coordinates (aux-code) are always consumed here.
 *
 * Tap: a touch that lasts at most tap_ms (BTN_TOUCH press to release on the left's clock) and
 * moves at most tap_move (accumulated |dx|+|dy| before acceleration) invokes the click binding:
 * press, click-ms, release, on the system work queue. The cursor moves normally meanwhile. A
 * trackpad button edge, a malformed aux event, a confirmed rim scroll, or a parameter change
 * (Studio or the &tgs key) during the touch cancels the click.
 *
 * Parameters: the ones in effect (`live`) and the stored ones (`saved`, settings key tcn/gst)
 * change only under params_mutex, from the Studio RPC thread or the key. &tgs only flips the
 * on/off flags, in effect and stored at once, so Studio always shows what the key did.
 *
 * Rim scroll (aux coordinates, 0..1024 on both axes, centre 512): only a touch whose first aux
 * sample lies in the rim band (radius >= (100 - rim_width)% of 512) is a candidate. The cursor
 * moves while the angle around the centre adds up in one direction; once it reaches
 * rim_start_deg the touch scrolls: its REL_X/REL_Y keep coming with value 0 (their sync is
 * kept) and every aux sample turns its angle step into REL_WHEEL (rim_steps per 90 degrees,
 * clockwise = down unless rim_reverse) on the listeners of wheel-device. Movement before that
 * point is not replayed as wheel. Moving inwards past the band's exit radius, an angle jump
 * over 90 degrees, moving on without a new aux sample for aux-timeout-ms (a resting finger
 * gets none and keeps scrolling), a layer of a listener override (the scroller layers), a
 * malformed aux event or the release end it for the rest of the touch. Aux samples follow
 * their REL frame, so a change applies from the next REL_X, and a frame's REL_X decides for
 * its REL_Y.
 *
 * Fail-open: when a gesturing touch gets no source event for contact-timeout-ms, it stops
 * gesturing, a forwarded BTN_TOUCH press is released once the way the right half ends a touch
 * (the late real release is dropped), and the gestures stay off until the next touch. The timer
 * only posts a marker event into the input queue, so all touch state lives on the input thread.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT toucan_input_gestures

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/iterable_sections.h>

#include <dt-bindings/toucan/touch_gestures.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

#include <toucan/touch_gestures.h>

LOG_MODULE_REGISTER(toucan_gestures, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "one toucan,input-gestures node: its parameters are global");

#define NODE DT_DRV_INST(0)
#define SELF DEVICE_DT_GET(NODE)
#define SOURCE DEVICE_DT_GET(DT_PHANDLE(NODE, device))
#define AUX_CODE DT_PROP(NODE, aux_code)
#define CONTACT_TIMEOUT_MS DT_PROP(NODE, contact_timeout_ms)
#define AUX_TIMEOUT_MS DT_PROP(NODE, aux_timeout_ms)
#define RIM_BUILT (AUX_CODE != 0 && DT_NODE_HAS_PROP(NODE, wheel_device))
#if DT_NODE_HAS_PROP(NODE, wheel_device)
#define WHEEL DEVICE_DT_GET(DT_PHANDLE(NODE, wheel_device))
#else
#define WHEEL NULL
#endif

#define CHECK_DT_DEFAULT(field, lo, hi)                                                            \
    BUILD_ASSERT(IN_RANGE(DT_PROP(NODE, field), lo, hi), "DT " #field " is out of range");
TCN_GESTURE_TAP_RANGES(CHECK_DT_DEFAULT)
TCN_GESTURE_RIM_RANGES(CHECK_DT_DEFAULT)
BUILD_ASSERT(!DT_PROP(NODE, rim) || RIM_BUILT, "rim scroll needs aux-code and wheel-device");

#define RIM_CENTER 512
#define RIM_RADIUS 512
#define RIM_JUMP 16384    /* 90 degrees in 1/65536 turns */
#define RIM_QUARTER 16384 /* wheel steps are counted per 90 degrees */
/* Net raw movement that always changes the right's 0..1024 position (1.75 / 1.375 counts per
 * unit), so an aux sample is due; the right skips unchanged positions (aux-abs-on-change). */
#define AUX_DUE_MOVE 2

/* Layers of the listener overrides (the scroller): rim scroll yields to them. */
#define TGS_LAYER_BIT(node, prop, idx) | BIT(DT_PROP_BY_IDX(node, prop, idx))
#define TGS_CHILD_LAYERS(child) DT_FOREACH_PROP_ELEM(child, layers, TGS_LAYER_BIT)
#define TGS_LISTENER_LAYERS(listener)                                                              \
    | (DT_SAME_NODE(DT_PHANDLE(listener, device), NODE)                                            \
           ? (0 DT_FOREACH_CHILD(listener, TGS_CHILD_LAYERS))                                      \
           : 0)
static const zmk_keymap_layers_state_t override_layers =
    0 DT_FOREACH_STATUS_OKAY(zmk_input_listener, TGS_LISTENER_LAYERS);

/* Watchdog marker: posted as an event of the source device and consumed here. */
#define MARKER_TYPE INPUT_EV_MSC
#define MARKER_CODE 0x7F01

/* A virtual key position after those of the input-processor behaviors. */
#define CLICK_POSITION                                                                             \
    ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(                                             \
        0, DT_NUM_INST_STATUS_OKAY(zmk_input_processor_behaviors))

#define DT_DEFAULTS                                                                                \
    {                                                                                              \
        .tap_ms = DT_PROP(NODE, tap_ms),                                                           \
        .tap_move = DT_PROP(NODE, tap_move),                                                       \
        .rim_width = DT_PROP(NODE, rim_width),                                                     \
        .rim_start_deg = DT_PROP(NODE, rim_start_deg),                                             \
        .rim_steps = DT_PROP(NODE, rim_steps),                                                     \
        .tap = DT_PROP(NODE, tap),                                                                 \
        .rim = DT_PROP(NODE, rim),                                                                 \
        .rim_reverse = DT_PROP(NODE, rim_reverse),                                                 \
    }

static const struct tcn_gesture_params dt_defaults = DT_DEFAULTS;
static const struct zmk_behavior_binding click_binding = ZMK_KEYMAP_EXTRACT_BINDING(0, NODE);

/* Written under params_mutex by the RPC and the key; the input thread copies `live` in
 * refresh() under the spinlock (it never takes the mutex, which may be held during a flash
 * write). */
static K_MUTEX_DEFINE(params_mutex);
static struct k_spinlock lock;
static struct tcn_gesture_params live = DT_DEFAULTS;
static struct tcn_gesture_params saved = DT_DEFAULTS;
static atomic_t generation = ATOMIC_INIT(1);
static atomic_t aux_seen;

/* Input thread only. */
static struct {
    struct tcn_gesture_params cfg; /* snapshot of `live` */
    atomic_val_t generation;
    bool on;           /* a gesture is enabled */
    bool touch;        /* a forwarded BTN_TOUCH press waits for its release */
    bool drop_release; /* fail-open released `touch`; drop its late real release */
    bool armed;        /* this touch is gesturing */
    bool tap;          /* this touch may still click */
    bool touch_aux;    /* this touch carried aux coordinates */
    bool zero_frame;   /* REL_X of this frame was zeroed: zero its REL_Y too */
    uint8_t contact;   /* armed touch counter, carried by the watchdog marker */
    uint16_t moved;
    uint32_t start_ms;
    uint32_t last_ms;
    enum { RIM_NONE, RIM_CANDIDATE, RIM_ACTIVE, RIM_DONE } rim;
    uint16_t rim_angle; /* last aux angle, 1/65536 turns */
    int32_t rim_acc;    /* candidate: angle moved in one direction */
    int32_t rim_rem;    /* active: wheel remainder, 1/RIM_QUARTER steps */
    uint32_t rim_in2;   /* squared radius where the band starts */
    uint32_t rim_out2;  /* squared radius below which the touch left the band */
    uint32_t rim_start; /* rim_start_deg in 1/65536 turns */
    int32_t aux_net[2]; /* active: raw REL_X/REL_Y since the last aux sample */
    bool aux_due;       /* active: the finger moved enough for a new aux sample */
    uint32_t aux_due_ms;
} st;

/* ---------------------------------------------------------------- click (system work queue) */

static atomic_t click_busy;
static bool click_down;

static int invoke_click(bool pressed) {
    const struct zmk_behavior_binding_event event = {
        .position = CLICK_POSITION,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    int err = zmk_behavior_invoke_binding(&click_binding, event, pressed);
    if (err < 0) {
        LOG_ERR("Tap click %s failed: %d", pressed ? "press" : "release", err);
    }
    return err;
}

static void click_release_cb(struct k_work *work) {
    if (click_down) {
        click_down = false;
        invoke_click(false);
    }
    atomic_clear(&click_busy);
}

static K_WORK_DELAYABLE_DEFINE(click_release_work, click_release_cb);

static void click_press_cb(struct k_work *work) {
    click_down = invoke_click(true) >= 0;
    k_work_schedule(&click_release_work, K_MSEC(DT_PROP(NODE, click_ms)));
}

static K_WORK_DEFINE(click_press_work, click_press_cb);

/* ---------------------------------------------------------------- input thread */

static void dispatch(const struct device *dev, uint8_t type, uint16_t code, int32_t value,
                     bool sync) {
    STRUCT_SECTION_FOREACH(input_listener, listener) {
        if (listener->dev == dev) {
            struct input_event evt = {
                .dev = dev, .type = type, .code = code, .value = value, .sync = sync};
            listener->callback(&evt);
        }
    }
}

static void forward(uint8_t type, uint16_t code, int32_t value, bool sync) {
    dispatch(SELF, type, code, value, sync);
}

static void watchdog_expired(struct k_timer *timer) {
    /* Timer context: hand the check to the input thread; retry while its queue is full. */
    const int32_t contact = (int32_t)(uintptr_t)k_timer_user_data_get(timer);
    if (input_report(SOURCE, MARKER_TYPE, MARKER_CODE, contact, false, K_NO_WAIT) < 0) {
        k_timer_start(timer, K_MSEC(10), K_NO_WAIT);
    }
}

K_TIMER_DEFINE(tcn_gesture_watchdog, watchdog_expired, NULL);

static void start_watchdog(uint32_t ms) {
    k_timer_user_data_set(&tcn_gesture_watchdog, (void *)(uintptr_t)st.contact);
    k_timer_start(&tcn_gesture_watchdog, K_MSEC(ms), K_NO_WAIT);
}

static void disarm(void) {
    st.armed = false;
    st.tap = false;
    st.rim = RIM_DONE;
    k_timer_stop(&tcn_gesture_watchdog);
}

static void refresh(void) {
    const atomic_val_t gen = atomic_get(&generation);
    if (gen == st.generation) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&lock);
    st.cfg = live;
    st.on = st.cfg.tap || (RIM_BUILT && st.cfg.rim);
    k_spin_unlock(&lock, key);
    st.generation = gen;
    if (st.armed) {
        LOG_DBG("Gesture settings changed during a touch; it stops gesturing");
        disarm();
    }
}

static void fail_open(uint32_t idle_ms) {
    LOG_WRN("Trackpad silent for %u ms during a touch: gestures off until the next touch", idle_ms);
    disarm();
    if (st.touch) {
        st.touch = false;
        st.drop_release = true;
        forward(INPUT_EV_KEY, INPUT_BTN_TOUCH, 0, false);
        forward(INPUT_EV_REL, INPUT_REL_X, 0, false);
        forward(INPUT_EV_REL, INPUT_REL_Y, 0, true);
    }
    if (atomic_get(&click_busy)) {
        k_work_reschedule(&click_release_work, K_NO_WAIT);
    }
}

static void on_marker(int32_t contact) {
    if (!st.armed || (uint8_t)contact != st.contact) {
        return;
    }
    const uint32_t idle_ms = k_uptime_get_32() - st.last_ms;
    if (idle_ms < CONTACT_TIMEOUT_MS) {
        start_watchdog(CONTACT_TIMEOUT_MS - idle_ms);
    } else {
        fail_open(idle_ms);
    }
}

static bool override_active(void) { return (zmk_keymap_layer_state() & override_layers) != 0; }

/* Angle of (x, y) in 1/65536 turns, 0 along +x and growing towards +y (clockwise with y down).
 * Octant reduction and atan(t) ~ t * pi/4 + 0.273 t (1 - t); error below 0.25 degrees. */
static uint16_t angle_of(int32_t x, int32_t y) {
    const int32_t ax = x < 0 ? -x : x;
    const int32_t ay = y < 0 ? -y : y;
    const bool steep = ay > ax;
    const int64_t t = ((int64_t)(steep ? ax : ay) << 15) / MAX(steep ? ay : ax, 1);
    int32_t angle = (int32_t)((8192 * t + ((2847 * t * (32768 - t)) >> 15)) >> 15);
    if (steep) {
        angle = 16384 - angle;
    }
    if (x < 0) {
        angle = 32768 - angle;
    }
    if (y < 0) {
        angle = 65536 - angle;
    }
    return (uint16_t)angle;
}

static void rim_end(void) {
    if (st.rim == RIM_ACTIVE) {
        LOG_DBG("Rim scroll ended");
    }
    st.rim = RIM_DONE;
}

static void rim_sample(uint32_t x, uint32_t y) {
    if (override_active()) {
        rim_end();
        return;
    }
    const int32_t dx = (int32_t)x - RIM_CENTER;
    const int32_t dy = (int32_t)y - RIM_CENTER;
    const uint32_t r2 = dx * dx + dy * dy;
    const uint16_t angle = angle_of(dx, dy);
    const int16_t step = (int16_t)(angle - st.rim_angle);
    st.rim_angle = angle;
    st.aux_net[0] = st.aux_net[1] = 0;
    st.aux_due = false;

    if (st.rim == RIM_NONE) {
        /* The touch's first coordinate decides whether it may scroll at all. */
        st.rim = r2 >= st.rim_in2 ? RIM_CANDIDATE : RIM_DONE;
        st.rim_acc = 0;
        return;
    }
    if (r2 < st.rim_out2 || abs(step) > RIM_JUMP) {
        rim_end();
        return;
    }
    if (st.rim == RIM_CANDIDATE) {
        if (step != 0) {
            const bool reversed = st.rim_acc != 0 && (step < 0) != (st.rim_acc < 0);
            st.rim_acc = reversed ? step : st.rim_acc + step;
        }
        if ((uint32_t)abs(st.rim_acc) >= st.rim_start) {
            LOG_DBG("Rim scroll starts after %d/65536 turn", st.rim_acc);
            st.rim = RIM_ACTIVE;
            st.rim_rem = 0;
            st.tap = false;
        }
        return;
    }
    const int32_t units = step * st.cfg.rim_steps + st.rim_rem;
    const int32_t wheel = units / RIM_QUARTER;
    st.rim_rem = units - wheel * RIM_QUARTER;
    if (wheel != 0 && RIM_BUILT) {
        dispatch(WHEEL, INPUT_EV_REL, INPUT_REL_WHEEL, st.cfg.rim_reverse ? wheel : -wheel, true);
    }
}

static void on_aux(int32_t value) {
    const uint32_t x = value & 0x7FF;
    const uint32_t y = (value >> 11) & 0x7FF;
    if ((value >> 22) || x > 1024 || y > 1024) {
        LOG_DBG("Malformed aux coordinate 0x%x", value);
        st.tap = false;
        rim_end();
        return;
    }
    if (!st.touch) {
        return; /* not a coordinate of the current touch */
    }
    st.touch_aux = true;
    atomic_set(&aux_seen, 1);
    if (st.armed && st.rim != RIM_DONE) {
        rim_sample(x, y);
    }
}

static void touch_pressed(uint32_t now) {
    if (st.armed) {
        disarm(); /* the previous touch ended without a release that arrived here */
    }
    st.touch = true;
    st.drop_release = false;
    st.touch_aux = false;
    if (!st.on) {
        return;
    }
    st.armed = true;
    st.tap = st.cfg.tap;
    st.contact++;
    st.moved = 0;
    st.start_ms = now;
    st.rim = RIM_DONE;
    if (RIM_BUILT && st.cfg.rim && !override_active()) {
        const int32_t r_in = RIM_RADIUS * (100 - st.cfg.rim_width) / 100;
        const int32_t r_out = r_in - (RIM_RADIUS - r_in) / 2;
        st.rim_in2 = r_in * r_in;
        st.rim_out2 = r_out * r_out;
        st.rim_start = (uint32_t)st.cfg.rim_start_deg * 65536 / 360;
        st.rim = RIM_NONE;
    }
    start_watchdog(CONTACT_TIMEOUT_MS);
}

/* Returns false when the release must be dropped. */
static bool touch_released(uint32_t now) {
    if (st.drop_release) {
        st.drop_release = false;
        return false;
    }
    st.touch = false;
    atomic_set(&aux_seen, st.touch_aux);
    if (!st.armed) {
        return true;
    }
    const uint32_t held_ms = now - st.start_ms;
    const bool click = st.tap && held_ms <= st.cfg.tap_ms;
    LOG_DBG("Touch %u ms, moved %u: %s", held_ms, st.moved, click ? "click" : "no click");
    disarm();
    if (click) {
        if (!atomic_cas(&click_busy, 0, 1)) {
            LOG_DBG("Previous tap click still running");
        } else if (k_work_submit(&click_press_work) < 0) {
            atomic_clear(&click_busy);
        }
    }
    return true;
}

static void on_move(int32_t value) {
    const uint32_t step = value < 0 ? -(uint32_t)value : (uint32_t)value;
    st.moved = MIN((uint32_t)st.moved + MIN(step, UINT16_MAX), UINT16_MAX);
    if (st.moved > st.cfg.tap_move) {
        st.tap = false;
    }
}

/* REL_X/REL_Y value to forward: zero while the rim scrolls, decided once per frame at REL_X.
 * Coordinates count as lost when the finger moved enough for a new aux sample (aux_due) and
 * none came for aux-timeout-ms; a resting finger sends none and keeps the scroll. */
static int32_t frame_value(const struct input_event *evt, uint32_t now) {
    if (st.rim == RIM_ACTIVE) {
        int32_t *net = &st.aux_net[evt->code == INPUT_REL_Y];
        *net = CLAMP(*net + evt->value, -AUX_DUE_MOVE, AUX_DUE_MOVE);
        if (!st.aux_due && (*net == AUX_DUE_MOVE || *net == -AUX_DUE_MOVE)) {
            st.aux_due = true;
            st.aux_due_ms = now;
        }
    }
    if (evt->code == INPUT_REL_X) {
        if (st.rim == RIM_ACTIVE &&
            ((st.aux_due && now - st.aux_due_ms > AUX_TIMEOUT_MS) || override_active())) {
            rim_end();
        }
        st.zero_frame = st.rim == RIM_ACTIVE;
    }
    const int32_t value = st.zero_frame ? 0 : evt->value;
    if (evt->code == INPUT_REL_Y) {
        st.zero_frame = false;
    }
    return value;
}

static void source_event(struct input_event *evt) {
    if (evt->type == MARKER_TYPE && evt->code == MARKER_CODE) {
        on_marker(evt->value);
        return;
    }
    refresh();
    const uint32_t now = k_uptime_get_32();
    st.last_ms = now;

    if (AUX_CODE && evt->type == INPUT_EV_ABS && evt->code == AUX_CODE) {
        on_aux(evt->value);
        return;
    }

    int32_t value = evt->value;
    if (evt->type == INPUT_EV_KEY) {
        if (evt->code != INPUT_BTN_TOUCH) {
            st.tap = false; /* the trackpad reported its own button: no second click */
        } else if (evt->value) {
            touch_pressed(now);
        } else if (!touch_released(now)) {
            return;
        }
    } else if (evt->type == INPUT_EV_REL &&
               (evt->code == INPUT_REL_X || evt->code == INPUT_REL_Y)) {
        if (st.armed) {
            on_move(evt->value);
        }
        value = frame_value(evt, now);
    }
    forward(evt->type, evt->code, value, evt->sync);
}

INPUT_CALLBACK_DEFINE(SOURCE, source_event);

/* ---------------------------------------------------------------- parameters (params_mutex) */

#define GESTURE_RECORD_VERSION 1

struct gesture_record {
    uint16_t version;
    struct tcn_gesture_params params;
};

BUILD_ASSERT(sizeof(struct gesture_record) == 12, "gesture record layout (version 1) changed");

bool tcn_gestures_params_equal(const struct tcn_gesture_params *a,
                               const struct tcn_gesture_params *b) {
    return a->tap == b->tap && a->tap_ms == b->tap_ms && a->tap_move == b->tap_move &&
           a->rim == b->rim && a->rim_width == b->rim_width &&
           a->rim_start_deg == b->rim_start_deg && a->rim_steps == b->rim_steps &&
           a->rim_reverse == b->rim_reverse;
}

static void set_live(const struct tcn_gesture_params *params) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    live = *params;
    k_spin_unlock(&lock, key);
    atomic_inc(&generation);
}

static struct tcn_gesture_params get_live(void) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    const struct tcn_gesture_params params = live;
    k_spin_unlock(&lock, key);
    return params;
}

static int store(const struct tcn_gesture_params *params) {
#if IS_ENABLED(CONFIG_SETTINGS)
    const struct gesture_record record = {.version = GESTURE_RECORD_VERSION, .params = *params};
    int err = settings_save_one(TOUCAN_SETTINGS_GESTURE, &record, sizeof(record));
    if (err) {
        return err;
    }
#endif
    saved = *params;
    return 0;
}

void tcn_gestures_defaults(struct tcn_gesture_params *params) { *params = dt_defaults; }

void tcn_gestures_get(struct tcn_gesture_params *current, struct tcn_gesture_params *stored) {
    k_mutex_lock(&params_mutex, K_FOREVER);
    *current = get_live();
    *stored = saved;
    k_mutex_unlock(&params_mutex);
}

void tcn_gestures_set(const struct tcn_gesture_params *params) {
    k_mutex_lock(&params_mutex, K_FOREVER);
    set_live(params);
    k_mutex_unlock(&params_mutex);
}

int tcn_gestures_save(void) {
    k_mutex_lock(&params_mutex, K_FOREVER);
    const struct tcn_gesture_params current = get_live();
    int err = tcn_gestures_params_equal(&current, &saved) ? 0 : store(&current);
    k_mutex_unlock(&params_mutex);
    return err;
}

void tcn_gestures_discard(void) {
    k_mutex_lock(&params_mutex, K_FOREVER);
    set_live(&saved);
    k_mutex_unlock(&params_mutex);
}

int tcn_gestures_restore_defaults(void) {
    int err = 0;
    k_mutex_lock(&params_mutex, K_FOREVER);
#if IS_ENABLED(CONFIG_SETTINGS)
    err = settings_delete(TOUCAN_SETTINGS_GESTURE);
#endif
    if (!err) {
        saved = dt_defaults;
        set_live(&dt_defaults);
    }
    k_mutex_unlock(&params_mutex);
    return err;
}

int tcn_gestures_switch(uint32_t op) {
    k_mutex_lock(&params_mutex, K_FOREVER);
    struct tcn_gesture_params current = get_live();
    bool on;
    switch (op) {
    case TGS_TOG:
        on = !(current.tap || current.rim);
        break;
    case TGS_ON:
        on = true;
        break;
    case TGS_OFF:
        on = false;
        break;
    default:
        k_mutex_unlock(&params_mutex);
        return -EINVAL;
    }
    current.tap = on;
    current.rim = on && tcn_gestures_rim_available();
    struct tcn_gesture_params stored = saved;
    stored.tap = current.tap;
    stored.rim = current.rim;
    set_live(&current);
    int err = tcn_gestures_params_equal(&stored, &saved) ? 0 : store(&stored);
    k_mutex_unlock(&params_mutex);
    LOG_INF("Touch gestures %s by key", on ? "on" : "off");
    if (err) {
        LOG_ERR("Storing the gesture settings failed: %d", err);
    }
    return err;
}

bool tcn_gestures_rim_available(void) { return RIM_BUILT; }

bool tcn_gestures_rim_samples_seen(void) { return atomic_get(&aux_seen) != 0; }

#if IS_ENABLED(CONFIG_SETTINGS)
static bool in_range(uint32_t value, uint32_t min, uint32_t max) {
    return value >= min && value <= max;
}

static int load_params(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (name && *name) {
        return -ENOENT;
    }
    struct gesture_record record;
    if (len != sizeof(record) || read_cb(cb_arg, &record, sizeof(record)) != sizeof(record) ||
        record.version != GESTURE_RECORD_VERSION) {
        LOG_WRN("Ignoring malformed or unsupported gesture settings record");
        return 0;
    }
#define CHECK(field, lo, hi)                                                                       \
    if (!in_range(record.params.field, lo, hi)) {                                                  \
        LOG_WRN("Ignoring gesture settings: invalid " #field);                                     \
        return 0;                                                                                  \
    }
    TCN_GESTURE_TAP_RANGES(CHECK)
    TCN_GESTURE_RIM_RANGES(CHECK)
#undef CHECK
    if (record.params.rim && !tcn_gestures_rim_available()) {
        LOG_WRN("Stored rim scroll is not built here; loading the rest");
        record.params.rim = false;
    }
    k_mutex_lock(&params_mutex, K_FOREVER);
    saved = record.params;
    set_live(&record.params);
    k_mutex_unlock(&params_mutex);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tcn_gst, TOUCAN_SETTINGS_GESTURE, NULL, load_params, NULL, NULL);
#endif

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                      CONFIG_TOUCAN_TOUCH_GESTURES_INIT_PRIORITY, NULL);

#if DT_NODE_HAS_PROP(NODE, wheel_device)
/* Rim scroll output: its listener (no processors) turns REL_WHEEL into HID scroll. */
DEVICE_DT_DEFINE(DT_PHANDLE(NODE, wheel_device), NULL, NULL, NULL, NULL, POST_KERNEL,
                 CONFIG_TOUCAN_TOUCH_GESTURES_INIT_PRIORITY, NULL);
#endif
