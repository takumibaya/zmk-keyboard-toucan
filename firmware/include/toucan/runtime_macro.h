/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zmk/behavior.h>

#define TCN_MACRO_SLOTS CONFIG_TOUCAN_STUDIO_MACRO_SLOTS
#define TCN_MACRO_STEPS CONFIG_TOUCAN_STUDIO_MACRO_MAX_STEPS
#define TCN_MACRO_QUEUE_ITEMS CONFIG_TOUCAN_STUDIO_MACRO_MAX_QUEUE_ITEMS

#define TCN_MACRO_WAIT_MAX 1000
#define TCN_MACRO_TAP_MAX 500
#define TCN_MACRO_RECORD_VERSION 1

enum tcn_macro_action {
    TCN_MACRO_TAP,
    TCN_MACRO_PRESS,
    TCN_MACRO_RELEASE,
    TCN_MACRO_PAUSE,
};

struct tcn_macro_step {
    uint32_t param1;
    uint32_t param2;
    uint16_t local_id;
    uint16_t wait_ms;
    uint16_t tap_ms;
    uint8_t action;
    uint8_t reserved;
};

struct tcn_macro_record {
    uint8_t version;
    uint8_t count;
    uint16_t reserved;
    struct tcn_macro_step steps[TCN_MACRO_STEPS];
};

extern struct k_mutex tcn_macro_mutex;
extern struct tcn_macro_record tcn_macro_active[TCN_MACRO_SLOTS];
extern uint8_t tcn_macro_pending_validation;

const char *tcn_runtime_macro_name(void);
struct zmk_behavior_binding tcn_macro_binding(const struct tcn_macro_step *step);
