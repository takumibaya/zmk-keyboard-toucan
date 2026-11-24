/*
 * Adapted from qtrnmr/zmk-module-runtime-config at 76703f3.
 * Copyright (c) 2026 qtrnmr and contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT toucan_behavior_runtime_macro

#include <string.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <toucan/runtime_macro.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

K_MUTEX_DEFINE(tcn_macro_mutex);
struct tcn_macro_record tcn_macro_active[TCN_MACRO_SLOTS];

static struct macro_run {
    struct k_work_delayable work;
    struct zmk_behavior_binding_event event;
    struct tcn_macro_step steps[TCN_MACRO_STEPS];
    uint8_t slot;
    uint8_t count;
    uint8_t next;
    uint8_t pause;
    bool parked;
    bool released;
    bool tap_release_pending;
} runs[CONFIG_TOUCAN_STUDIO_MACRO_HELD];

const char *tcn_runtime_macro_name(void) {
    return DEVICE_DT_GET(DT_NODELABEL(rtm))->name;
}

struct zmk_behavior_binding tcn_macro_binding(const struct tcn_macro_step *step) {
    return (struct zmk_behavior_binding){
        .behavior_dev = zmk_behavior_find_behavior_name_from_local_id(step->local_id),
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
        .local_id = step->local_id,
#endif
        .param1 = step->param1,
        .param2 = step->param2,
    };
}

static void invoke_step(const struct macro_run *run, const struct tcn_macro_step *step,
                        bool pressed) {
    struct zmk_behavior_binding binding = tcn_macro_binding(step);
    struct zmk_behavior_binding_event event = run->event;
    event.timestamp = k_uptime_get();
    zmk_behavior_invoke_binding(&binding, event, pressed);
}

static void execute_macro(struct k_work *work) {
    struct macro_run *run =
        CONTAINER_OF(k_work_delayable_from_work(work), struct macro_run, work);

    while (run->next < run->count) {
        if (run->next == run->pause) {
            k_mutex_lock(&tcn_macro_mutex, K_FOREVER);
            bool released = run->released;
            run->parked = !released;
            k_mutex_unlock(&tcn_macro_mutex);
            if (!released) {
                return;
            }
            run->next++;
            continue;
        }

        const struct tcn_macro_step *step = &run->steps[run->next];
        uint16_t delay;
        if (step->action == TCN_MACRO_TAP && !run->tap_release_pending) {
            invoke_step(run, step, true);
            run->tap_release_pending = true;
            delay = step->tap_ms;
        } else {
            invoke_step(run, step, step->action == TCN_MACRO_PRESS);
            run->tap_release_pending = false;
            run->next++;
            delay = step->wait_ms;
        }
        if (delay) {
            k_work_schedule(&run->work, K_MSEC(delay));
            return;
        }
    }

    k_mutex_lock(&tcn_macro_mutex, K_FOREVER);
    run->count = 0;
    k_mutex_unlock(&tcn_macro_mutex);
}

static bool matches_press(const struct macro_run *run, uint32_t slot,
                          const struct zmk_behavior_binding_event *event) {
    return run->count && !run->released && run->slot == slot &&
           run->event.position == event->position
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
           && run->event.source == event->source
#endif
        ;
}

static int macro_pressed(struct zmk_behavior_binding *binding,
                         struct zmk_behavior_binding_event event) {
    uint32_t slot = binding->param1;
    if (slot >= TCN_MACRO_SLOTS) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    k_mutex_lock(&tcn_macro_mutex, K_FOREVER);
    const struct tcn_macro_record *record = &tcn_macro_active[slot];
    if (!record->count || (tcn_macro_pending_validation & BIT(slot))) {
        k_mutex_unlock(&tcn_macro_mutex);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    struct macro_run *run = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(runs); i++) {
        if (matches_press(&runs[i], slot, &event)) {
            k_mutex_unlock(&tcn_macro_mutex);
            return ZMK_BEHAVIOR_OPAQUE;
        }
        if (!run && !runs[i].count) {
            run = &runs[i];
        }
    }
    if (!run) {
        k_mutex_unlock(&tcn_macro_mutex);
        LOG_WRN("No executor available for runtime macro %u", slot);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    run->slot = slot;
    run->event = event;
    run->count = record->count;
    run->next = 0;
    run->pause = record->count;
    run->parked = false;
    run->released = false;
    run->tap_release_pending = false;
    memcpy(run->steps, record->steps, record->count * sizeof(run->steps[0]));
    for (uint8_t i = 0; i < record->count; i++) {
        if (run->steps[i].action == TCN_MACRO_PAUSE) {
            run->pause = i;
            break;
        }
    }
    k_mutex_unlock(&tcn_macro_mutex);
    k_work_schedule(&run->work, K_NO_WAIT);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int macro_released(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    k_mutex_lock(&tcn_macro_mutex, K_FOREVER);
    for (size_t i = 0; i < ARRAY_SIZE(runs); i++) {
        struct macro_run *run = &runs[i];
        if (matches_press(run, binding->param1, &event)) {
            run->released = true;
            if (run->parked) {
                k_work_schedule(&run->work, K_NO_WAIT);
            }
            break;
        }
    }
    k_mutex_unlock(&tcn_macro_mutex);
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata slot_value = {
    .display_name = "Slot",
    .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
    .range = {.min = 0, .max = TCN_MACRO_SLOTS - 1},
};

static const struct behavior_parameter_metadata_set macro_metadata_set = {
    .param1_values_len = 1,
    .param1_values = &slot_value,
};

static const struct behavior_parameter_metadata macro_metadata = {
    .sets_len = 1,
    .sets = &macro_metadata_set,
};
#endif

static const struct behavior_driver_api macro_api = {
    .binding_pressed = macro_pressed,
    .binding_released = macro_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &macro_metadata,
#endif
};

static int macro_init(const struct device *dev) {
    for (size_t i = 0; i < ARRAY_SIZE(runs); i++) {
        k_work_init_delayable(&runs[i].work, execute_macro);
    }
    return 0;
}

BEHAVIOR_DT_DEFINE(DT_NODELABEL(rtm), macro_init, NULL, NULL, NULL, POST_KERNEL,
                   CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &macro_api);
