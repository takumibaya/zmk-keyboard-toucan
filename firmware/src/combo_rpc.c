/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <drivers/behavior.h>
#include <zmk/combos.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

#include <toucan/studio_rpc.h>

LOG_MODULE_DECLARE(toucan_studio, CONFIG_ZMK_LOG_LEVEL);

#define MAX_KEYS CONFIG_ZMK_COMBOS_RUNTIME_MAX_KEYS
#define PAGE_SIZE ARRAY_SIZE(((toucan_studio_ComboPage *)0)->combos)
#define TIMEOUT_MAX 2000
#define PRIOR_IDLE_MAX 2000
#define LAYER_MASK BIT_MASK(ZMK_KEYMAP_LAYERS_LEN)
#define ERROR(name) toucan_studio_ErrorCode_ERROR_CODE_##name
#define FLAG(name) toucan_studio_ComboFlag_COMBO_FLAG_##name

BUILD_ASSERT(ZMK_COMBOS_LEN <= UINT8_MAX);
BUILD_ASSERT(ZMK_KEYMAP_LEN <= UINT8_MAX);
BUILD_ASSERT(ZMK_KEYMAP_LAYERS_LEN <= 16);
BUILD_ASSERT(MAX_KEYS <= ARRAY_SIZE(((toucan_studio_Combo *)0)->positions));

#define RECORD_VERSION 1
#define RECORD_DELETED BIT(0)
#define RECORD_SLOW_RELEASE BIT(1)
#define RECORD_FROM_DT BIT(2)

/* All multibyte values are little-endian; the stored format has no ABI padding. */
struct combo_record {
    uint8_t version;
    uint8_t flags;
    uint8_t count;
    uint8_t dt_crc[2];
    uint8_t timeout[2];
    uint8_t prior_idle[2];
    uint8_t layers[2];
    uint8_t local_id[2];
    uint8_t param1[4];
    uint8_t param2[4];
    uint8_t positions[6];
};

static struct zmk_combo_config saved[ZMK_COMBOS_LEN];
static struct zmk_combo_config staged[ZMK_COMBOS_LEN];
static struct combo_record pending[ZMK_COMBOS_LEN];
static bool stored[UINT8_MAX + 1];
static atomic_t boot_loading = ATOMIC_INIT(1);

static bool from_dt(size_t slot) { return slot < zmk_combo_dt_count(); }

static bool protected(const struct zmk_combo_config *cfg) {
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_studio_unlock)
#define MATCH_UNLOCK(node)                                                                          \
    if (strcmp(cfg->behavior.behavior_dev, DEVICE_DT_NAME(node)) == 0) {                             \
        return true;                                                                              \
    }
    if (cfg->behavior.behavior_dev) {
        DT_FOREACH_STATUS_OKAY(zmk_behavior_studio_unlock, MATCH_UNLOCK)
    }
#undef MATCH_UNLOCK
#endif
    return false;
}

static bool same_keys(const struct zmk_combo_config *a, const struct zmk_combo_config *b) {
    if (a->key_position_len != b->key_position_len) {
        return false;
    }
    for (size_t i = 0; i < a->key_position_len; i++) {
        size_t j = 0;
        while (j < b->key_position_len && b->key_positions[j] != a->key_positions[i]) {
            j++;
        }
        if (j == b->key_position_len) {
            return false;
        }
    }
    return true;
}

static bool configs_equal(const struct zmk_combo_config *a, const struct zmk_combo_config *b) {
    if (!a->key_position_len || !b->key_position_len) {
        return a->key_position_len == b->key_position_len;
    }
    return a->timeout_ms == b->timeout_ms && a->require_prior_idle_ms == b->require_prior_idle_ms &&
           a->layer_mask == b->layer_mask && a->slow_release == b->slow_release &&
           a->behavior.param1 == b->behavior.param1 && a->behavior.param2 == b->behavior.param2 &&
           (a->behavior.behavior_dev == b->behavior.behavior_dev ||
            strcmp(a->behavior.behavior_dev, b->behavior.behavior_dev) == 0) &&
           same_keys(a, b);
}

static bool conflicts(const struct zmk_combo_config *a, const struct zmk_combo_config *b) {
    return a->key_position_len && b->key_position_len &&
           (!a->layer_mask || !b->layer_mask || (a->layer_mask & b->layer_mask)) && same_keys(a, b);
}

static toucan_studio_Error validate(struct zmk_combo_config *cfg, bool has_binding,
                                     uint16_t local_id) {
    if (cfg->key_position_len < 2 || cfg->key_position_len > MAX_KEYS) {
        return (toucan_studio_Error){ERROR(OUT_OF_RANGE), toucan_studio_Combo_positions_tag, 0};
    }
    for (size_t i = 0; i < cfg->key_position_len; i++) {
        if (cfg->key_positions[i] >= ZMK_KEYMAP_LEN) {
            return (toucan_studio_Error){ERROR(OUT_OF_RANGE), toucan_studio_Combo_positions_tag, i};
        }
        for (size_t j = 0; j < i; j++) {
            if (cfg->key_positions[j] == cfg->key_positions[i]) {
                return (toucan_studio_Error){ERROR(INVALID), toucan_studio_Combo_positions_tag, i};
            }
        }
    }
    if (!has_binding || local_id == UINT16_MAX) {
        return (toucan_studio_Error){ERROR(INVALID), toucan_studio_Combo_binding_tag, 0};
    }
    cfg->behavior.behavior_dev = zmk_behavior_find_behavior_name_from_local_id(local_id);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
    cfg->behavior.local_id = local_id;
#endif
    if (!cfg->behavior.behavior_dev || zmk_behavior_validate_binding(&cfg->behavior) < 0) {
        return (toucan_studio_Error){ERROR(INVALID), toucan_studio_Combo_binding_tag, 0};
    }
    if (cfg->timeout_ms < 1 || cfg->timeout_ms > TIMEOUT_MAX) {
        return (toucan_studio_Error){ERROR(OUT_OF_RANGE), toucan_studio_Combo_timeout_ms_tag, 0};
    }
    if (cfg->require_prior_idle_ms < -1 || cfg->require_prior_idle_ms > PRIOR_IDLE_MAX) {
        return (toucan_studio_Error){ERROR(OUT_OF_RANGE),
                                    toucan_studio_Combo_require_prior_idle_ms_tag, 0};
    }
    if (cfg->layer_mask & ~LAYER_MASK) {
        return (toucan_studio_Error){ERROR(OUT_OF_RANGE), toucan_studio_Combo_layer_mask_tag, 0};
    }
    return (toucan_studio_Error){0};
}

static void fill_combo(toucan_studio_Combo *out, size_t slot,
                       const struct zmk_combo_config *cfg) {
    struct zmk_combo_config def;
    zmk_combo_read_default(slot, &def);
    *out = (toucan_studio_Combo){.slot = slot};
    if (from_dt(slot)) {
        out->flags |= FLAG(FROM_DT);
        if (protected(&def)) {
            out->flags |= FLAG(PROTECTED);
        }
        if (!cfg->key_position_len) {
            out->flags |= FLAG(DELETED);
        }
    }
    if (!from_dt(slot) || !configs_equal(cfg, &def)) {
        out->flags |= FLAG(MODIFIED);
    }
    if (!configs_equal(cfg, &saved[slot])) {
        out->flags |= FLAG(UNSAVED);
    }
    if (cfg->key_position_len) {
        out->positions_count = cfg->key_position_len;
        for (size_t i = 0; i < cfg->key_position_len; i++) {
            out->positions[i] = cfg->key_positions[i];
        }
        out->has_binding = true;
        out->binding = (toucan_studio_Binding){
            .behavior_id = zmk_behavior_get_local_id(cfg->behavior.behavior_dev),
            .param1 = cfg->behavior.param1,
            .param2 = cfg->behavior.param2,
        };
        out->timeout_ms = cfg->timeout_ms;
        out->require_prior_idle_ms = cfg->require_prior_idle_ms;
        out->layer_mask = cfg->layer_mask;
        out->slow_release = cfg->slow_release;
    }
}

void tcn_combo_info(toucan_studio_ComboInfo *info) {
    *info = (toucan_studio_ComboInfo){
        .slots = zmk_combo_slot_count(),
        .max_keys = MAX_KEYS,
        .key_positions = ZMK_KEYMAP_LEN,
        .page_size = PAGE_SIZE,
        .layers = ZMK_KEYMAP_LAYERS_LEN,
        .has_timeout_ms = true,
        .timeout_ms = {.min = 1, .max = TIMEOUT_MAX},
        .has_prior_idle_ms = true,
        .prior_idle_ms = {.min = 0, .max = PRIOR_IDLE_MAX},
    };
}

void tcn_combo_list(const toucan_studio_ListCombosRequest *req, toucan_studio_Response *resp) {
    if (atomic_get(&boot_loading)) {
        tcn_rpc_error(resp, ERROR(BUSY), 0, 0);
        return;
    }
    size_t total = zmk_combo_slot_count();
    if (req->start_slot > total) {
        tcn_rpc_error(resp, ERROR(OUT_OF_RANGE), toucan_studio_ListCombosRequest_start_slot_tag, 0);
        return;
    }
    resp->which_result = toucan_studio_Response_combos_tag;
    toucan_studio_ComboPage *page = &resp->result.combos;
    *page = (toucan_studio_ComboPage){.total_slots = total, .next_slot = total};
    for (size_t slot = req->start_slot; slot < total; slot++) {
        struct zmk_combo_config cfg;
        zmk_combo_read(slot, &cfg);
        if (!cfg.key_position_len && !from_dt(slot)) {
            continue;
        }
        if (page->combos_count == PAGE_SIZE) {
            page->next_slot = slot;
            break;
        }
        fill_combo(&page->combos[page->combos_count++], slot, &cfg);
    }
}

static bool check_slot(toucan_studio_Response *resp, size_t slot, uint8_t field) {
    struct zmk_combo_config def;
    if (zmk_combo_read_default(slot, &def) < 0) {
        tcn_rpc_error(resp, ERROR(OUT_OF_RANGE), field, 0);
        return false;
    }
    if (protected(&def)) {
        tcn_rpc_error(resp, ERROR(PROTECTED), field, 0);
        return false;
    }
    if (atomic_get(&boot_loading)) {
        tcn_rpc_error(resp, ERROR(BUSY), 0, 0);
        return false;
    }
    return true;
}

static void update_slot(toucan_studio_Response *resp, size_t slot,
                        const struct zmk_combo_config *cfg) {
    int rc = zmk_combo_set(slot, cfg);
    if (rc < 0) {
        tcn_rpc_error(resp, tcn_rpc_errno_to_code(rc), 0, 0);
        return;
    }
    resp->which_result = toucan_studio_Response_combo_tag;
    fill_combo(&resp->result.combo, slot, cfg);
}

void tcn_combo_set(const toucan_studio_SetComboRequest *req, toucan_studio_Response *resp) {
    const toucan_studio_Combo *combo = &req->combo;
    if (!req->has_combo) {
        tcn_rpc_error(resp, ERROR(INVALID), toucan_studio_SetComboRequest_combo_tag, 0);
        return;
    }
    if (!check_slot(resp, combo->slot, toucan_studio_Combo_slot_tag)) {
        return;
    }
    struct zmk_combo_config cfg = {
        .key_position_len = combo->positions_count,
        .timeout_ms = combo->timeout_ms,
        .require_prior_idle_ms = combo->require_prior_idle_ms,
        .layer_mask = combo->layer_mask,
        .slow_release = combo->slow_release,
        .behavior = {.param1 = combo->binding.param1, .param2 = combo->binding.param2},
    };
    for (size_t i = 0; i < MIN(cfg.key_position_len, MAX_KEYS); i++) {
        cfg.key_positions[i] = combo->positions[i];
    }
    toucan_studio_Error err = validate(&cfg, combo->has_binding, combo->binding.behavior_id);
    if (err.code) {
        tcn_rpc_error(resp, err.code, err.field, err.index);
        return;
    }
    for (size_t slot = 0; slot < zmk_combo_slot_count(); slot++) {
        if (slot == combo->slot) {
            continue;
        }
        struct zmk_combo_config other;
        zmk_combo_read(slot, &other);
        if (conflicts(&cfg, &other)) {
            tcn_rpc_error(resp, ERROR(DUPLICATE), toucan_studio_Combo_positions_tag, 0);
            return;
        }
    }
    update_slot(resp, combo->slot, &cfg);
}

void tcn_combo_delete(const toucan_studio_SlotRequest *req, toucan_studio_Response *resp) {
    if (check_slot(resp, req->slot, toucan_studio_SlotRequest_slot_tag)) {
        update_slot(resp, req->slot, &(const struct zmk_combo_config){0});
    }
}

void tcn_combo_restore(const toucan_studio_SlotRequest *req, toucan_studio_Response *resp) {
    if (!check_slot(resp, req->slot, toucan_studio_SlotRequest_slot_tag)) {
        return;
    }
    struct zmk_combo_config def;
    zmk_combo_read_default(req->slot, &def);
    for (size_t slot = 0; def.key_position_len && slot < zmk_combo_slot_count(); slot++) {
        if (slot == req->slot) {
            continue;
        }
        struct zmk_combo_config other, other_def;
        zmk_combo_read(slot, &other);
        zmk_combo_read_default(slot, &other_def);
        if (conflicts(&def, &other) && !configs_equal(&other, &other_def)) {
            tcn_rpc_error(resp, ERROR(DUPLICATE), toucan_studio_SlotRequest_slot_tag, 0);
            return;
        }
    }
    update_slot(resp, req->slot, &def);
}

bool tcn_combo_unsaved(void) {
    if (atomic_get(&boot_loading)) {
        return false;
    }
    for (size_t slot = 0; slot < zmk_combo_slot_count(); slot++) {
        struct zmk_combo_config cfg;
        zmk_combo_read(slot, &cfg);
        if (!configs_equal(&cfg, &saved[slot])) {
            return true;
        }
    }
    return false;
}

static uint16_t default_crc(const struct zmk_combo_config *def) {
    uint8_t positions[MAX_KEYS];
    for (size_t i = 0; i < def->key_position_len; i++) {
        size_t j = i;
        while (j && positions[j - 1] > def->key_positions[i]) {
            positions[j] = positions[j - 1];
            j--;
        }
        positions[j] = def->key_positions[i];
    }
    uint8_t params[8];
    sys_put_le32(def->behavior.param1, params);
    sys_put_le32(def->behavior.param2, params + 4);
    uint16_t crc = crc16_ansi(positions, def->key_position_len);
    crc = crc16_reflect(0xa001, crc, params, sizeof(params));
    return crc16_reflect(0xa001, crc, (const uint8_t *)def->behavior.behavior_dev,
                         strlen(def->behavior.behavior_dev) + 1);
}

static int persist(size_t slot, const struct zmk_combo_config *cfg) {
    char key[sizeof(TOUCAN_SETTINGS_COMBO) + sizeof("/255") - 1];
    snprintf(key, sizeof(key), TOUCAN_SETTINGS_COMBO "/%u", (unsigned int)slot);
    struct zmk_combo_config def;
    if (!cfg || (zmk_combo_read_default(slot, &def) == 0 && configs_equal(cfg, &def))) {
        if (!stored[slot]) {
            return 0;
        }
        int rc = settings_delete(key);
        if (!rc) {
            stored[slot] = false;
        }
        return rc;
    }
    struct combo_record rec = {.version = RECORD_VERSION, .count = cfg->key_position_len};
    if (from_dt(slot)) {
        rec.flags |= RECORD_FROM_DT;
        sys_put_le16(default_crc(&def), rec.dt_crc);
    }
    if (!cfg->key_position_len) {
        rec.flags |= RECORD_DELETED;
    } else {
        if (cfg->slow_release) {
            rec.flags |= RECORD_SLOW_RELEASE;
        }
        sys_put_le16(cfg->timeout_ms, rec.timeout);
        sys_put_le16(cfg->require_prior_idle_ms, rec.prior_idle);
        sys_put_le16(cfg->layer_mask, rec.layers);
        sys_put_le16(zmk_behavior_get_local_id(cfg->behavior.behavior_dev), rec.local_id);
        sys_put_le32(cfg->behavior.param1, rec.param1);
        sys_put_le32(cfg->behavior.param2, rec.param2);
        for (size_t i = 0; i < cfg->key_position_len; i++) {
            rec.positions[i] = cfg->key_positions[i];
        }
    }
    int rc = settings_save_one(key, &rec, sizeof(rec));
    if (!rc) {
        stored[slot] = true;
    }
    return rc;
}

int tcn_combo_save(void) {
    if (atomic_get(&boot_loading)) {
        return -EBUSY;
    }
    int first_err = 0;
    for (size_t slot = 0; slot < zmk_combo_slot_count(); slot++) {
        struct zmk_combo_config cfg;
        zmk_combo_read(slot, &cfg);
        if (configs_equal(&cfg, &saved[slot])) {
            continue;
        }
        int rc = persist(slot, &cfg);
        if (!rc) {
            saved[slot] = cfg;
        } else if (!first_err) {
            first_err = rc;
        }
    }
    return first_err;
}

int tcn_combo_discard(void) {
    return atomic_get(&boot_loading) ? -EBUSY : zmk_combo_apply(saved);
}

int tcn_combo_restore_defaults(void) {
    if (atomic_get(&boot_loading)) {
        return -EBUSY;
    }
    for (size_t slot = 0; slot < zmk_combo_slot_count(); slot++) {
        zmk_combo_read_default(slot, &staged[slot]);
    }
    int rc = zmk_combo_apply(staged);
    if (rc < 0) {
        return rc;
    }
    int first_err = 0;
    for (size_t slot = 0; slot < ARRAY_SIZE(stored); slot++) {
        rc = persist(slot, NULL);
        if (!rc && slot < zmk_combo_slot_count()) {
            saved[slot] = staged[slot];
        } else if (rc && !first_err) {
            first_err = rc;
        }
    }
    return first_err;
}

static int combo_settings_set(const char *key, size_t len, settings_read_cb read_cb, void *arg) {
    unsigned int slot = 0;
    if (!key || !*key || (key[0] == '0' && key[1])) {
        return 0;
    }
    for (; *key; key++) {
        if (*key < '0' || *key > '9') {
            return 0;
        }
        slot = slot * 10 + (*key - '0');
        if (slot > UINT8_MAX) {
            return 0;
        }
    }
    stored[slot] = true;
    if (slot < zmk_combo_slot_count()) {
        struct combo_record *rec = &pending[slot];
        if (len != sizeof(*rec) || read_cb(arg, rec, len) != sizeof(*rec)) {
            rec->version = 0;
        }
    }
    return 0;
}

static bool decode_record(size_t slot, const struct zmk_combo_config *def,
                          struct zmk_combo_config *out) {
    const struct combo_record *rec = &pending[slot];
    bool dt = from_dt(slot);
    if (rec->version != RECORD_VERSION || protected(def) ||
        (rec->flags & ~(RECORD_FROM_DT | RECORD_DELETED | RECORD_SLOW_RELEASE)) ||
        (dt != !!(rec->flags & RECORD_FROM_DT)) ||
        (dt && sys_get_le16(rec->dt_crc) != default_crc(def))) {
        return false;
    }
    if (rec->flags & RECORD_DELETED) {
        if (!dt || rec->count || (rec->flags & RECORD_SLOW_RELEASE)) {
            return false;
        }
        *out = (struct zmk_combo_config){0};
        return true;
    }
    struct zmk_combo_config cfg = {
        .key_position_len = rec->count,
        .timeout_ms = sys_get_le16(rec->timeout),
        .require_prior_idle_ms = (int16_t)sys_get_le16(rec->prior_idle),
        .layer_mask = sys_get_le16(rec->layers),
        .slow_release = (rec->flags & RECORD_SLOW_RELEASE) != 0,
        .behavior = {.param1 = sys_get_le32(rec->param1), .param2 = sys_get_le32(rec->param2)},
    };
    for (size_t i = 0; i < MIN(cfg.key_position_len, MAX_KEYS); i++) {
        cfg.key_positions[i] = rec->positions[i];
    }
    if (validate(&cfg, true, sys_get_le16(rec->local_id)).code) {
        return false;
    }
    *out = cfg;
    return true;
}

static void apply_loaded_combos(struct k_work *work) {
    int rc = zmk_combo_apply(staged);
    if (rc == -EBUSY) {
        k_work_schedule(k_work_delayable_from_work(work), K_MSEC(50));
        return;
    }
    if (rc < 0) {
        LOG_WRN("Could not apply stored combos: %d", rc);
    } else {
        memcpy(saved, staged, sizeof(saved));
    }
    atomic_clear(&boot_loading);
}

K_WORK_DELAYABLE_DEFINE(boot_apply, apply_loaded_combos);

static int combo_settings_commit(void) {
    bool accepted[ZMK_COMBOS_LEN] = {0};
    size_t slots = zmk_combo_slot_count();
    for (size_t slot = 0; slot < slots; slot++) {
        zmk_combo_read_default(slot, &saved[slot]);
        staged[slot] = saved[slot];
        accepted[slot] = stored[slot] && decode_record(slot, &saved[slot], &staged[slot]) &&
                         !configs_equal(&staged[slot], &saved[slot]);
    }
    /* Check the complete saved table, so valid swaps and delete + replace survive reboot.
     * Reverting a rejected override can reintroduce a DT chord: check again until stable.
     */
    bool rejected;
    do {
        rejected = false;
        for (size_t i = 0; i < slots; i++) {
            for (size_t j = i + 1; j < slots; j++) {
                if ((accepted[i] || accepted[j]) && conflicts(&staged[i], &staged[j])) {
                    size_t slot = accepted[j] ? j : i;
                    accepted[slot] = false;
                    staged[slot] = saved[slot];
                    rejected = true;
                }
            }
        }
    } while (rejected);
    bool has_overrides = false;
    for (size_t slot = 0; slot < ARRAY_SIZE(stored); slot++) {
        if (slot < slots) {
            pending[slot].version = 0;
            if (accepted[slot]) {
                has_overrides = true;
                continue;
            }
        }
        if (stored[slot]) {
            LOG_WRN("Dropping invalid or redundant stored combo %u", (unsigned int)slot);
            int rc = persist(slot, NULL);
            if (rc < 0) {
                LOG_WRN("Could not erase stored combo %u: %d", (unsigned int)slot, rc);
            }
        }
    }
    /* settings_load holds its mutex in h_commit; do not wait on the system queue here. */
    if (has_overrides) {
        k_work_schedule(&boot_apply, K_NO_WAIT);
    } else {
        atomic_clear(&boot_loading);
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(toucan_combo, TOUCAN_SETTINGS_COMBO, NULL, combo_settings_set,
                               combo_settings_commit, NULL);
