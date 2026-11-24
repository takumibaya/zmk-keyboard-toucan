/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT toucan_input_processor_runtime_scaler

#include <drivers/input_processor.h>
#include <toucan/runtime_scaler.h>
#include <zephyr/sys/atomic.h>

struct scaler_config {
    uint8_t type;
    size_t codes_len;
    uint16_t codes[];
};

struct scaler_data {
    atomic_t ratio;
    atomic_t active;
};

#define PACK_RATIO(mul, div) (((mul) << 8) | (div))

struct tcn_scaler_ratio tcn_scaler_get_ratio(const struct device *dev) {
    struct scaler_data *data = dev->data;
    atomic_val_t packed = atomic_get(&data->ratio);
    return (struct tcn_scaler_ratio){.mul = packed >> 8, .div = packed & UINT8_MAX};
}

int tcn_scaler_set_ratio(const struct device *dev, struct tcn_scaler_ratio ratio) {
    if (!ratio.mul || !ratio.div) {
        return -ERANGE;
    }
    struct scaler_data *data = dev->data;
    atomic_set(&data->ratio, PACK_RATIO(ratio.mul, ratio.div));
    return 0;
}

bool tcn_scaler_active(const struct device *dev) {
    struct scaler_data *data = dev->data;
    return atomic_get(&data->active) != 0;
}

static int scaler_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                               uint32_t param2, struct zmk_input_processor_state *state) {
    const struct scaler_config *cfg = dev->config;
    if (event->type != cfg->type) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    for (size_t i = 0; i < cfg->codes_len; i++) {
        if (event->code != cfg->codes[i]) {
            continue;
        }
        struct scaler_data *data = dev->data;
        if (!atomic_get(&data->active)) {
            atomic_set(&data->active, 1);
        }

        struct tcn_scaler_ratio ratio = tcn_scaler_get_ratio(dev);
        int16_t value_mul = event->value * (int16_t)ratio.mul;
        if (state && state->remainder) {
            value_mul += *state->remainder;
        }
        int16_t scaled = value_mul / (int16_t)ratio.div;
        if (state && state->remainder) {
            *state->remainder = value_mul - scaled * (int16_t)ratio.div;
        }
        event->value = scaled;
        break;
    }
    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api scaler_driver_api = {
    .handle_event = scaler_handle_event,
};

#define SCALER_INST(n)                                                                            \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP(n, default_mul), 1, UINT8_MAX));                              \
    BUILD_ASSERT(IN_RANGE(DT_INST_PROP(n, default_div), 1, UINT8_MAX));                              \
    static struct scaler_data scaler_data_##n = {                                                  \
        .ratio = ATOMIC_INIT(PACK_RATIO(DT_INST_PROP(n, default_mul),                               \
                                       DT_INST_PROP(n, default_div))),                            \
    };                                                                                            \
    static const struct scaler_config scaler_config_##n = {                                        \
        .type = DT_INST_PROP(n, type),                                                             \
        .codes_len = DT_INST_PROP_LEN(n, codes),                                                   \
        .codes = DT_INST_PROP(n, codes),                                                           \
    };                                                                                            \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &scaler_data_##n, &scaler_config_##n, POST_KERNEL,          \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &scaler_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SCALER_INST)
