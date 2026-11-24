/*
 * Adapted from qtrnmr/zmk-module-runtime-config at 76703f3.
 * Copyright (c) 2026 qtrnmr and contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT toucan_behavior_runtime_macro

#include <string.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior_queue.h>
#include <toucan/runtime_macro.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

K_MUTEX_DEFINE(tcn_macro_mutex);
struct tcn_macro_record tcn_macro_active[TCN_MACRO_SLOTS];

/* Preserve the release tail even when Studio replaces or clears its slot mid-press. */
static struct held_macro {
    uint32_t position;
    uint8_t slot;
    uint8_t count;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    struct tcn_macro_step steps[TCN_MACRO_STEPS - 1];
} held[TCN_MACRO_SLOTS];
static bool queueing;

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

static void enqueue(const struct zmk_behavior_binding_event *event,
                    const struct tcn_macro_step *steps, uint8_t count) {
    queueing = true;
    for (uint8_t i = 0; i < count; i++) {
        const struct tcn_macro_step *step = &steps[i];
        struct zmk_behavior_binding binding = tcn_macro_binding(step);
        int err;
        if (step->action == TCN_MACRO_TAP) {
            err = zmk_behavior_queue_add(event, binding, true, step->tap_ms);
            if (!err) {
                err = zmk_behavior_queue_add(event, binding, false, step->wait_ms);
            }
        } else {
            err = zmk_behavior_queue_add(event, binding, step->action == TCN_MACRO_PRESS,
                                         step->wait_ms);
        }
        if (err) {
            LOG_WRN("Runtime macro queue failed at step %u: %d", i, err);
            break;
        }
    }
    queueing = false;
}

static bool is_held(const struct held_macro *entry, uint32_t slot,
                    const struct zmk_behavior_binding_event *event) {
    return entry->count && entry->slot == slot && entry->position == event->position
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
           && entry->source == event->source
#endif
        ;
}

static void start_macro(uint32_t slot, const struct zmk_behavior_binding_event *event) {
    const struct tcn_macro_record *record = &tcn_macro_active[slot];
    uint8_t pause = 0;
    while (pause < record->count && record->steps[pause].action != TCN_MACRO_PAUSE) {
        pause++;
    }
    if (pause + 1 < record->count) {
        struct held_macro *entry = NULL;
        for (size_t i = 0; i < ARRAY_SIZE(held); i++) {
            if (is_held(&held[i], slot, event)) {
                return;
            }
            if (!entry && !held[i].count) {
                entry = &held[i];
            }
        }
        if (!entry) {
            LOG_WRN("No release snapshot available for runtime macro %u", slot);
            return;
        }
        entry->position = event->position;
        entry->slot = slot;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        entry->source = event->source;
#endif
        entry->count = record->count - pause - 1;
        memcpy(entry->steps, &record->steps[pause + 1], entry->count * sizeof(entry->steps[0]));
    }
    enqueue(event, record->steps, pause);
}

static int run_macro(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event,
                     bool pressed) {
    if (binding->param1 >= TCN_MACRO_SLOTS) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    k_mutex_lock(&tcn_macro_mutex, K_FOREVER);
    /* A queue add can invoke a behavior synchronously before it returns. */
    if (!queueing) {
        if (pressed) {
            start_macro(binding->param1, &event);
        } else {
            for (size_t i = 0; i < ARRAY_SIZE(held); i++) {
                if (is_held(&held[i], binding->param1, &event)) {
                    enqueue(&event, held[i].steps, held[i].count);
                    held[i].count = 0;
                    break;
                }
            }
        }
    }
    k_mutex_unlock(&tcn_macro_mutex);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int macro_pressed(struct zmk_behavior_binding *binding,
                         struct zmk_behavior_binding_event event) {
    return run_macro(binding, event, true);
}

static int macro_released(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    return run_macro(binding, event, false);
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

BEHAVIOR_DT_DEFINE(DT_NODELABEL(rtm), NULL, NULL, NULL, NULL, POST_KERNEL,
                   CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &macro_api);
