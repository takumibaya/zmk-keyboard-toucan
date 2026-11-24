/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <toucan/runtime_macro.h>
#include <toucan/studio_rpc.h>

LOG_MODULE_DECLARE(toucan_studio, CONFIG_ZMK_LOG_LEVEL);

#define MACRO_ERROR(name) toucan_studio_ErrorCode_ERROR_CODE_##name
#define MACRO_ACTION(name) toucan_studio_MacroAction_MACRO_ACTION_##name
#define RECORD_HEADER_SIZE offsetof(struct tcn_macro_record, steps)
#define CHUNK_STEPS ARRAY_SIZE(((toucan_studio_MacroSteps *)0)->steps)
#define WIRE_SLOTS ARRAY_SIZE(((toucan_studio_MacroList *)0)->macros)

BUILD_ASSERT(TCN_MACRO_SLOTS <= WIRE_SLOTS && WIRE_SLOTS <= 10);
BUILD_ASSERT(CHUNK_STEPS == ARRAY_SIZE(((toucan_studio_SetMacroChunkRequest *)0)->steps));
BUILD_ASSERT(TCN_MACRO_STEPS <= 32);
BUILD_ASSERT(TCN_MACRO_QUEUE_ITEMS <= CONFIG_ZMK_BEHAVIORS_QUEUE_SIZE);
BUILD_ASSERT(TCN_MACRO_QUEUE_ITEMS <= UINT8_MAX);
BUILD_ASSERT(RECORD_HEADER_SIZE == 4 && sizeof(struct tcn_macro_step) == 16);
BUILD_ASSERT((int)TCN_MACRO_TAP == (int)MACRO_ACTION(TAP) &&
             (int)TCN_MACRO_PRESS == (int)MACRO_ACTION(PRESS) &&
             (int)TCN_MACRO_RELEASE == (int)MACRO_ACTION(RELEASE) &&
             (int)TCN_MACRO_PAUSE == (int)MACRO_ACTION(PAUSE_FOR_RELEASE));

struct write_key {
    uint16_t seq;
    uint8_t total;
    bool valid;
};

/* Only the Studio thread touches these, except the saved cache during settings load. */
static struct tcn_macro_record saved[TCN_MACRO_SLOTS];
static struct write_key committed[TCN_MACRO_SLOTS];
static struct {
    struct write_key key;
    uint8_t slot;
    uint32_t received;
    struct tcn_macro_record record;
} staging;

static size_t record_size(uint8_t count) {
    return RECORD_HEADER_SIZE + count * sizeof(struct tcn_macro_step);
}

static unsigned int step_queue_items(uint8_t action) {
    return action == TCN_MACRO_PAUSE ? 0 : action == TCN_MACRO_TAP ? 2 : 1;
}

static unsigned int queue_items(const struct tcn_macro_record *record) {
    unsigned int items = 0;
    for (uint8_t i = 0; i < record->count; i++) {
        items += step_queue_items(record->steps[i].action);
    }
    return items;
}

static bool slot_unsaved(uint8_t slot) {
    const struct tcn_macro_record *active = &tcn_macro_active[slot];
    return active->count != saved[slot].count ||
           memcmp(active->steps, saved[slot].steps,
                  active->count * sizeof(active->steps[0])) != 0;
}

static void replace_slot(uint8_t slot, const struct tcn_macro_record *record) {
    k_mutex_lock(&tcn_macro_mutex, K_FOREVER);
    if (record) {
        memcpy(&tcn_macro_active[slot], record, record_size(record->count));
    } else {
        tcn_macro_active[slot].count = 0;
    }
    k_mutex_unlock(&tcn_macro_mutex);
}

static bool valid_binding(const struct tcn_macro_step *step) {
    struct zmk_behavior_binding binding = tcn_macro_binding(step);
    return binding.behavior_dev && strcmp(binding.behavior_dev, tcn_runtime_macro_name()) != 0 &&
           zmk_behavior_validate_binding(&binding) == 0;
}

static bool validate_step(const toucan_studio_MacroStep *wire, struct tcn_macro_step *step,
                          toucan_studio_Response *resp, uint8_t index) {
    if ((unsigned int)wire->action > TCN_MACRO_PAUSE) {
        tcn_rpc_error(resp, MACRO_ERROR(INVALID), toucan_studio_MacroStep_action_tag, index);
        return false;
    }
    if (wire->wait_ms > TCN_MACRO_WAIT_MAX) {
        tcn_rpc_error(resp, MACRO_ERROR(OUT_OF_RANGE), toucan_studio_MacroStep_wait_ms_tag, index);
        return false;
    }
    if (wire->tap_ms > TCN_MACRO_TAP_MAX) {
        tcn_rpc_error(resp, MACRO_ERROR(OUT_OF_RANGE), toucan_studio_MacroStep_tap_ms_tag, index);
        return false;
    }
    *step = (struct tcn_macro_step){
        .action = wire->action, .wait_ms = wire->wait_ms, .tap_ms = wire->tap_ms,
    };
    if (step->action != TCN_MACRO_PAUSE) {
        step->local_id = wire->binding.behavior_id;
        step->param1 = wire->binding.param1;
        step->param2 = wire->binding.param2;
        if (!wire->has_binding || !valid_binding(step)) {
            tcn_rpc_error(resp, MACRO_ERROR(INVALID), toucan_studio_MacroStep_binding_tag, index);
            return false;
        }
    }
    return true;
}

static int duplicate_pause(const struct tcn_macro_record *record) {
    bool seen_pause = false;
    for (uint8_t i = 0; i < record->count; i++) {
        if (record->steps[i].action == TCN_MACRO_PAUSE) {
            if (seen_pause) {
                return i;
            }
            seen_pause = true;
        }
    }
    return -1;
}

void tcn_macro_info(toucan_studio_MacroInfo *info) {
    *info = (toucan_studio_MacroInfo){
        .slots = TCN_MACRO_SLOTS,
        .max_steps = TCN_MACRO_STEPS,
        .chunk_steps = CHUNK_STEPS,
        .max_queue_items = TCN_MACRO_QUEUE_ITEMS,
        .has_wait_ms = true,
        .wait_ms = {.min = 0, .max = TCN_MACRO_WAIT_MAX},
        .has_tap_ms = true,
        .tap_ms = {.min = 0, .max = TCN_MACRO_TAP_MAX},
        .behavior_id = zmk_behavior_get_local_id(tcn_runtime_macro_name()),
    };
}

void tcn_macro_list(toucan_studio_Response *resp) {
    resp->which_result = toucan_studio_Response_macros_tag;
    toucan_studio_MacroList *list = &resp->result.macros;
    list->macros_count = TCN_MACRO_SLOTS;
    for (uint8_t i = 0; i < TCN_MACRO_SLOTS; i++) {
        list->macros[i] = (toucan_studio_MacroSummary){
            .slot = i,
            .steps = tcn_macro_active[i].count,
            .queue_items = queue_items(&tcn_macro_active[i]),
            .unsaved = slot_unsaved(i),
        };
    }
}

void tcn_macro_get(const toucan_studio_GetMacroRequest *req, toucan_studio_Response *resp) {
    if (req->slot >= TCN_MACRO_SLOTS) {
        tcn_rpc_error(resp, MACRO_ERROR(OUT_OF_RANGE), toucan_studio_GetMacroRequest_slot_tag, 0);
        return;
    }
    const struct tcn_macro_record *record = &tcn_macro_active[req->slot];
    if (req->offset > record->count) {
        tcn_rpc_error(resp, MACRO_ERROR(OUT_OF_RANGE), toucan_studio_GetMacroRequest_offset_tag, 0);
        return;
    }
    resp->which_result = toucan_studio_Response_macro_tag;
    toucan_studio_MacroSteps *out = &resp->result.macro;
    out->slot = req->slot;
    out->total = record->count;
    out->offset = req->offset;
    out->steps_count = MIN(record->count - req->offset, (int)CHUNK_STEPS);
    for (pb_size_t i = 0; i < out->steps_count; i++) {
        const struct tcn_macro_step *step = &record->steps[req->offset + i];
        out->steps[i] = (toucan_studio_MacroStep){
            .action = step->action,
            .has_binding = step->action != TCN_MACRO_PAUSE,
            .binding = {.behavior_id = step->local_id, .param1 = step->param1,
                        .param2 = step->param2},
            .wait_ms = step->wait_ms,
            .tap_ms = step->tap_ms,
        };
    }
}

static void write_result(toucan_studio_Response *resp, uint8_t slot, uint16_t seq, uint8_t received,
                         uint8_t total, bool done) {
    resp->which_result = toucan_studio_Response_macro_write_tag;
    resp->result.macro_write = (toucan_studio_MacroWriteResult){
        .slot = slot, .seq = seq, .received = received, .total = total, .committed = done,
    };
}

static bool matches(const struct write_key *key, const toucan_studio_SetMacroChunkRequest *req) {
    return key->valid && key->seq == req->seq && key->total == req->total;
}

void tcn_macro_set_chunk(const toucan_studio_SetMacroChunkRequest *req,
                         toucan_studio_Response *resp) {
    if (req->slot >= TCN_MACRO_SLOTS) {
        tcn_rpc_error(resp, MACRO_ERROR(OUT_OF_RANGE), toucan_studio_SetMacroChunkRequest_slot_tag, 0);
        return;
    }
    if (req->total > TCN_MACRO_STEPS) {
        tcn_rpc_error(resp, MACRO_ERROR(OUT_OF_RANGE),
                      toucan_studio_SetMacroChunkRequest_total_tag, 0);
        return;
    }
    if (matches(&committed[req->slot], req)) {
        write_result(resp, req->slot, req->seq, req->total, req->total, true);
        return;
    }
    if (req->offset > req->total) {
        tcn_rpc_error(resp, MACRO_ERROR(OUT_OF_RANGE),
                      toucan_studio_SetMacroChunkRequest_offset_tag, 0);
        return;
    }
    if (req->steps_count > req->total - req->offset) {
        tcn_rpc_error(resp, MACRO_ERROR(OUT_OF_RANGE),
                      toucan_studio_SetMacroChunkRequest_steps_tag, 0);
        return;
    }

    if (staging.slot != req->slot || !matches(&staging.key, req)) {
        staging.slot = req->slot;
        staging.key = (struct write_key){.seq = req->seq, .total = req->total, .valid = true};
        staging.received = 0;
        staging.record.version = TCN_MACRO_RECORD_VERSION;
        staging.record.count = req->total;
    }

    struct tcn_macro_step chunk[CHUNK_STEPS];
    for (pb_size_t i = 0; i < req->steps_count; i++) {
        if (!validate_step(&req->steps[i], &chunk[i], resp, req->offset + i)) {
            return;
        }
    }
    for (pb_size_t i = 0; i < req->steps_count; i++) {
        uint8_t index = req->offset + i;
        staging.record.steps[index] = chunk[i];
        staging.received |= BIT(index);
    }

    uint8_t received = POPCOUNT(staging.received);
    bool done = received == req->total;
    if (done) {
        int pause = duplicate_pause(&staging.record);
        if (pause >= 0) {
            tcn_rpc_error(resp, MACRO_ERROR(INVALID), toucan_studio_MacroStep_action_tag, pause);
            return;
        }
        if (queue_items(&staging.record) > TCN_MACRO_QUEUE_ITEMS) {
            tcn_rpc_error(resp, MACRO_ERROR(NO_SPACE), toucan_studio_SetMacroChunkRequest_steps_tag, 0);
            return;
        }
        replace_slot(req->slot, &staging.record);
        committed[req->slot] = staging.key;
        staging.key.valid = false;
    }
    write_result(resp, req->slot, req->seq, received, req->total, done);
}

void tcn_macro_clear(const toucan_studio_SlotRequest *req, toucan_studio_Response *resp) {
    if (req->slot >= TCN_MACRO_SLOTS) {
        tcn_rpc_error(resp, MACRO_ERROR(OUT_OF_RANGE), toucan_studio_SlotRequest_slot_tag, 0);
        return;
    }
    if (staging.slot == req->slot) {
        staging.key.valid = false;
    }
    replace_slot(req->slot, NULL);
    write_result(resp, req->slot, 0, 0, 0, true);
}

bool tcn_macro_unsaved(void) {
    for (uint8_t i = 0; i < TCN_MACRO_SLOTS; i++) {
        if (slot_unsaved(i)) {
            return true;
        }
    }
    return false;
}

static int persist_slot(uint8_t slot, const struct tcn_macro_record *record) {
    char key[sizeof(TOUCAN_SETTINGS_MACRO "/255")];
    snprintf(key, sizeof(key), TOUCAN_SETTINGS_MACRO "/%u", slot);
    return record && record->count ? settings_save_one(key, record, record_size(record->count))
                                   : settings_delete(key);
}

int tcn_macro_save(void) {
    int first_error = 0;
    for (uint8_t i = 0; i < TCN_MACRO_SLOTS; i++) {
        if (!slot_unsaved(i)) {
            continue;
        }
        const struct tcn_macro_record *record = &tcn_macro_active[i];
        int err = persist_slot(i, record);
        if (!err) {
            memcpy(&saved[i], record, record_size(record->count));
        } else if (!first_error) {
            first_error = err;
        }
    }
    return first_error;
}

int tcn_macro_discard(void) {
    staging.key.valid = false;
    k_mutex_lock(&tcn_macro_mutex, K_FOREVER);
    for (uint8_t i = 0; i < TCN_MACRO_SLOTS; i++) {
        if (slot_unsaved(i)) {
            memcpy(&tcn_macro_active[i], &saved[i], record_size(saved[i].count));
        }
    }
    k_mutex_unlock(&tcn_macro_mutex);
    return 0;
}

int tcn_macro_restore_defaults(void) {
    staging.key.valid = false;
    k_mutex_lock(&tcn_macro_mutex, K_FOREVER);
    for (uint8_t i = 0; i < TCN_MACRO_SLOTS; i++) {
        tcn_macro_active[i].count = 0;
    }
    k_mutex_unlock(&tcn_macro_mutex);

    int first_error = 0;
    /* Also erase slots saved by firmware built with a larger slot count. */
    for (uint8_t i = 0; i < WIRE_SLOTS; i++) {
        int err = persist_slot(i, NULL);
        if (!err && i < TCN_MACRO_SLOTS) {
            saved[i].count = 0;
        } else if (err && !first_error) {
            first_error = err;
        }
    }
    return first_error;
}

static int macro_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *arg) {
    if (!name || name[0] < '0' || name[0] >= '0' + TCN_MACRO_SLOTS || name[1] != '\0') {
        LOG_WRN("Ignoring invalid runtime macro settings key");
        return 0;
    }
    uint8_t slot = name[0] - '0';
    struct tcn_macro_record *record = &saved[slot];
    if (len < RECORD_HEADER_SIZE || len > sizeof(*record) || read_cb(arg, record, len) != len ||
        record->version != TCN_MACRO_RECORD_VERSION || record->count > TCN_MACRO_STEPS ||
        record->reserved || len != record_size(record->count)) {
        LOG_WRN("Ignoring invalid runtime macro record %u", slot);
        record->count = 0;
    }
    return 0;
}

static bool valid_record(const struct tcn_macro_record *record) {
    for (uint8_t i = 0; i < record->count; i++) {
        const struct tcn_macro_step *step = &record->steps[i];
        if (step->reserved || step->action > TCN_MACRO_PAUSE || step->wait_ms > TCN_MACRO_WAIT_MAX ||
            step->tap_ms > TCN_MACRO_TAP_MAX ||
            (step->action != TCN_MACRO_PAUSE && !valid_binding(step))) {
            return false;
        }
        if (step->action == TCN_MACRO_PAUSE && (step->local_id || step->param1 || step->param2)) {
            return false;
        }
    }
    return duplicate_pause(record) < 0 && queue_items(record) <= TCN_MACRO_QUEUE_ITEMS;
}

static int macro_settings_commit(void) {
    for (uint8_t i = 0; i < TCN_MACRO_SLOTS; i++) {
        if (!valid_record(&saved[i])) {
            LOG_WRN("Ignoring invalid runtime macro steps in slot %u", i);
            saved[i].count = 0;
        }
        replace_slot(i, &saved[i]);
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(toucan_macro, TOUCAN_SETTINGS_MACRO, NULL, macro_settings_set,
                               macro_settings_commit, NULL);
