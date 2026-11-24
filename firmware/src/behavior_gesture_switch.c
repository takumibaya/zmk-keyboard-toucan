/*
 * &tgs: on/off key of the touch gestures (param1 TGS_TOG / TGS_ON / TGS_OFF). It turns every
 * built gesture on or off in the Studio settings and stores that at once; see
 * tcn_gestures_switch() in src/touch_gestures.c.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT toucan_behavior_gesture_switch

#include <drivers/behavior.h>
#include <dt-bindings/toucan/touch_gestures.h>
#include <zmk/behavior.h>

#include <toucan/touch_gestures.h>

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1, "one &tgs behavior");

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata param_values[] = {
    {.display_name = "Toggle", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = TGS_TOG},
    {.display_name = "On", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = TGS_ON},
    {.display_name = "Off", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = TGS_OFF},
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    int err = tcn_gestures_switch(binding->param1);
    return err < 0 ? err : ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_gesture_switch_driver_api = {
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_gesture_switch_driver_api);
