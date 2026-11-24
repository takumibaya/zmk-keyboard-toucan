/*
 * Touch gestures of the left half: tap-to-click on the right half's trackpad.
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
 * trackpad button edge, a malformed aux event, or a parameter or key change during the touch
 * cancels the click.
 *
 * Fail-open: when a gesturing touch gets no source event for contact-timeout-ms, it stops
 * gesturing, a forwarded BTN_TOUCH press is released once the way the right half ends a touch
 * (the late real release is dropped), and the gestures stay off until the next touch. The timer
 * only posts a marker event into the input queue, so all touch state lives on the input thread.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT toucan_input_gestures

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
             "one toucan,input-gestures node: its parameters and key are global");

#define NODE DT_DRV_INST(0)
#define SELF DEVICE_DT_GET(NODE)
#define SOURCE DEVICE_DT_GET(DT_PHANDLE(NODE, device))
#define AUX_CODE DT_PROP(NODE, aux_code)
#define CONTACT_TIMEOUT_MS DT_PROP(NODE, contact_timeout_ms)

#define CHECK_DT_DEFAULT(field, lo, hi)                                                            \
    BUILD_ASSERT(IN_RANGE(DT_PROP(NODE, field), lo, hi), "DT " #field " is out of range");
TCN_GESTURE_TAP_RANGES(CHECK_DT_DEFAULT)
TCN_GESTURE_RIM_RANGES(CHECK_DT_DEFAULT)
BUILD_ASSERT(!DT_PROP(NODE, rim), "rim scroll is not built");

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

/* Written by the RPC and the key, picked up by the input thread in refresh(). */
static struct k_spinlock lock;
static struct tcn_gesture_params live = DT_DEFAULTS;
static bool key_off;
static atomic_t generation = ATOMIC_INIT(1);
static atomic_t aux_seen;

/* Input thread only. */
static struct {
    struct tcn_gesture_params cfg; /* snapshot of `live` */
    atomic_val_t generation;
    bool on;           /* the key is on and a gesture is enabled */
    bool touch;        /* a forwarded BTN_TOUCH press waits for its release */
    bool drop_release; /* fail-open released `touch`; drop its late real release */
    bool armed;        /* this touch is gesturing */
    bool tap;          /* this touch may still click */
    bool touch_aux;    /* this touch carried aux coordinates */
    uint8_t contact;   /* armed touch counter, carried by the watchdog marker */
    uint16_t moved;
    uint32_t start_ms;
    uint32_t last_ms;
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

static void forward(uint8_t type, uint16_t code, int32_t value, bool sync) {
    STRUCT_SECTION_FOREACH(input_listener, listener) {
        if (listener->dev == SELF) {
            struct input_event evt = {
                .dev = SELF, .type = type, .code = code, .value = value, .sync = sync};
            listener->callback(&evt);
        }
    }
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
    k_timer_stop(&tcn_gesture_watchdog);
}

static void refresh(void) {
    const atomic_val_t gen = atomic_get(&generation);
    if (gen == st.generation) {
        return;
    }
    k_spinlock_key_t key = k_spin_lock(&lock);
    st.cfg = live;
    st.on = !key_off && st.cfg.tap;
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

static void on_aux(int32_t value) {
    const uint32_t x = value & 0x7FF;
    const uint32_t y = (value >> 11) & 0x7FF;
    if ((value >> 22) || x > 1024 || y > 1024) {
        LOG_DBG("Malformed aux coordinate 0x%x", value);
        st.tap = false;
        return;
    }
    if (st.touch) {
        st.touch_aux = true;
        atomic_set(&aux_seen, 1);
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

    if (evt->type == INPUT_EV_KEY) {
        if (evt->code != INPUT_BTN_TOUCH) {
            st.tap = false; /* the trackpad reported its own button: no second click */
        } else if (evt->value) {
            touch_pressed(now);
        } else if (!touch_released(now)) {
            return;
        }
    } else if (st.armed && evt->type == INPUT_EV_REL &&
               (evt->code == INPUT_REL_X || evt->code == INPUT_REL_Y)) {
        on_move(evt->value);
    }
    forward(evt->type, evt->code, evt->value, evt->sync);
}

INPUT_CALLBACK_DEFINE(SOURCE, source_event);

/* ---------------------------------------------------------------- parameters and key */

void tcn_gestures_defaults(struct tcn_gesture_params *params) { *params = dt_defaults; }

void tcn_gestures_get(struct tcn_gesture_params *params) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    *params = live;
    k_spin_unlock(&lock, key);
}

void tcn_gestures_set(const struct tcn_gesture_params *params) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    live = *params;
    k_spin_unlock(&lock, key);
    atomic_inc(&generation);
}

bool tcn_gestures_rim_available(void) { return false; }

bool tcn_gestures_rim_samples_seen(void) { return atomic_get(&aux_seen) != 0; }

bool tcn_gestures_switched_off(void) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    const bool off = key_off;
    k_spin_unlock(&lock, key);
    return off;
}

static void set_key_off(bool off) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    key_off = off;
    k_spin_unlock(&lock, key);
    atomic_inc(&generation);
}

#define SWITCH_RECORD_VERSION 1

struct switch_record {
    uint8_t version;
    uint8_t off;
};

static void switch_save_cb(struct k_work *work) {
#if IS_ENABLED(CONFIG_SETTINGS)
    const struct switch_record record = {.version = SWITCH_RECORD_VERSION,
                                         .off = tcn_gestures_switched_off()};
    int err = record.off ? settings_save_one(TOUCAN_SETTINGS_GESTURE_SWITCH, &record,
                                             sizeof(record))
                         : settings_delete(TOUCAN_SETTINGS_GESTURE_SWITCH);
    if (err) {
        LOG_ERR("Storing the gesture key state failed: %d", err);
    }
#endif
}

static K_WORK_DEFINE(switch_save_work, switch_save_cb);

int tcn_gestures_switch(uint32_t op) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    switch (op) {
    case TGS_TOG:
        key_off = !key_off;
        break;
    case TGS_ON:
        key_off = false;
        break;
    case TGS_OFF:
        key_off = true;
        break;
    default:
        k_spin_unlock(&lock, key);
        return -EINVAL;
    }
    const bool off = key_off;
    k_spin_unlock(&lock, key);
    atomic_inc(&generation);
    LOG_INF("Touch gestures %s by key", off ? "off" : "on");
    k_work_submit(&switch_save_work);
    return 0;
}

int tcn_gestures_switch_reset(void) {
    set_key_off(false);
#if IS_ENABLED(CONFIG_SETTINGS)
    return settings_delete(TOUCAN_SETTINGS_GESTURE_SWITCH);
#else
    return 0;
#endif
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int switch_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    if (name && *name) {
        return -ENOENT;
    }
    struct switch_record record;
    if (len != sizeof(record) || read_cb(cb_arg, &record, sizeof(record)) != sizeof(record) ||
        record.version != SWITCH_RECORD_VERSION) {
        LOG_WRN("Ignoring malformed or unsupported gesture key record");
        return 0;
    }
    set_key_off(record.off != 0);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tcn_gsw, TOUCAN_SETTINGS_GESTURE_SWITCH, NULL, switch_settings_set,
                               NULL, NULL);
#endif

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                      CONFIG_TOUCAN_TOUCH_GESTURES_INIT_PRIORITY, NULL);
