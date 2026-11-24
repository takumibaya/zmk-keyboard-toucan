/* SPDX-License-Identifier: MIT */

#pragma once

#include <zephyr/device.h>

struct tcn_scaler_ratio {
    uint8_t mul;
    uint8_t div;
};

struct tcn_scaler_ratio tcn_scaler_get_ratio(const struct device *dev);
int tcn_scaler_set_ratio(const struct device *dev, struct tcn_scaler_ratio ratio);
bool tcn_scaler_active(const struct device *dev);
