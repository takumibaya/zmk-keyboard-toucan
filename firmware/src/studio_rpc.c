/*
 * Custom Studio RPC subsystem "toucan__studio" (see proto/toucan/studio.proto).
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <pb_decode.h>

#include <zmk/studio/custom.h>
#include <zmk/studio/rpc.h>

#include <toucan/studio_rpc.h>

LOG_MODULE_REGISTER(toucan_studio, CONFIG_ZMK_LOG_LEVEL);

/*
 * Worst-case bytes that zmk_studio_Response adds around the custom payload:
 * request_response tag+len (3), request_id (6), custom=100 tag+len (4),
 * call tag+len (3), subsystem_index (2), payload tag+len (3).
 */
#define TOUCAN_RPC_ENVELOPE_MAX 21
#define TOUCAN_RPC_FRAME_MAX 256

BUILD_ASSERT(toucan_studio_Response_size + TOUCAN_RPC_ENVELOPE_MAX <= TOUCAN_RPC_FRAME_MAX,
             "a toucan__studio response could exceed 256 bytes");
BUILD_ASSERT(toucan_studio_Request_size <=
                 CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_REQUEST_PAYLOAD_MAX_BYTES,
             "raise CONFIG_ZMK_STUDIO_RPC_CUSTOM_SUBSYSTEM_REQUEST_PAYLOAD_MAX_BYTES");

#define BUILT_FEATURES                                                                             \
    ((IS_ENABLED(CONFIG_TOUCAN_STUDIO_COMBO) ? toucan_studio_Feature_FEATURE_COMBO : 0) |          \
     (IS_ENABLED(CONFIG_TOUCAN_STUDIO_MACRO) ? toucan_studio_Feature_FEATURE_MACRO : 0) |          \
     (IS_ENABLED(CONFIG_TOUCAN_STUDIO_POINTER) ? toucan_studio_Feature_FEATURE_POINTER : 0) |      \
     (IS_ENABLED(CONFIG_TOUCAN_STUDIO_GESTURE) ? toucan_studio_Feature_FEATURE_GESTURE : 0))

static struct zmk_rpc_custom_subsystem_meta toucan_studio_meta = {
    .ui_urls = NULL,
    .ui_urls_count = 0,
    .security = ZMK_STUDIO_RPC_HANDLER_SECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(toucan__studio, &toucan_studio_meta, toucan_studio_handle);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(toucan__studio, toucan_studio_Response);

void tcn_rpc_error(toucan_studio_Response *resp, toucan_studio_ErrorCode code, uint8_t field,
                   uint8_t index) {
    resp->which_result = toucan_studio_Response_error_tag;
    resp->result.error = (toucan_studio_Error){.code = code, .field = field, .index = index};
}

toucan_studio_ErrorCode tcn_rpc_errno_to_code(int err) {
    switch (err) {
    case -EBUSY:
        return toucan_studio_ErrorCode_ERROR_CODE_BUSY;
    case -ENOENT:
        return toucan_studio_ErrorCode_ERROR_CODE_NOT_FOUND;
    case -ENOTSUP:
        return toucan_studio_ErrorCode_ERROR_CODE_NOT_SUPPORTED;
    case -ERANGE:
        return toucan_studio_ErrorCode_ERROR_CODE_OUT_OF_RANGE;
    case -EINVAL:
        return toucan_studio_ErrorCode_ERROR_CODE_INVALID;
    default:
        return toucan_studio_ErrorCode_ERROR_CODE_STORAGE;
    }
}

static void fill_info(toucan_studio_Info *info) {
    *info = (toucan_studio_Info)toucan_studio_Info_init_zero;
    info->protocol_version = TOUCAN_STUDIO_PROTOCOL_VERSION;
    info->features = BUILT_FEATURES;
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_COMBO)
    info->has_combo = true;
    tcn_combo_info(&info->combo);
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_MACRO)
    info->has_macro = true;
    tcn_macro_info(&info->macro);
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_POINTER)
    info->has_pointer = true;
    tcn_pointer_info(&info->pointer);
#endif
}

static uint32_t unsaved_features(void) {
    uint32_t unsaved = 0;
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_COMBO)
    if (tcn_combo_unsaved()) {
        unsaved |= toucan_studio_Feature_FEATURE_COMBO;
    }
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_MACRO)
    if (tcn_macro_unsaved()) {
        unsaved |= toucan_studio_Feature_FEATURE_MACRO;
    }
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_POINTER)
    if (tcn_pointer_unsaved()) {
        unsaved |= toucan_studio_Feature_FEATURE_POINTER;
    }
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_GESTURE)
    if (tcn_gesture_unsaved()) {
        unsaved |= toucan_studio_Feature_FEATURE_GESTURE;
    }
#endif
    return unsaved;
}

static void fill_changes(toucan_studio_Response *resp) {
    resp->which_result = toucan_studio_Response_changes_tag;
    resp->result.changes = (toucan_studio_Changes){.unsaved = unsaved_features()};
}

enum mask_op { MASK_SAVE, MASK_DISCARD, MASK_RESTORE_DEFAULTS };

static int keep_first_error(int first, int err) { return first ? first : err; }

/* Runs the op for every requested feature and returns the first error (0 when all succeed). */
static int run_mask_op(enum mask_op op, uint32_t features) {
    int first_err = 0;
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_COMBO)
    if (features & toucan_studio_Feature_FEATURE_COMBO) {
        first_err = keep_first_error(first_err, op == MASK_SAVE      ? tcn_combo_save()
                                                : op == MASK_DISCARD ? tcn_combo_discard()
                                                                     : tcn_combo_restore_defaults());
    }
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_MACRO)
    if (features & toucan_studio_Feature_FEATURE_MACRO) {
        first_err = keep_first_error(first_err, op == MASK_SAVE      ? tcn_macro_save()
                                                : op == MASK_DISCARD ? tcn_macro_discard()
                                                                     : tcn_macro_restore_defaults());
    }
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_POINTER)
    if (features & toucan_studio_Feature_FEATURE_POINTER) {
        first_err =
            keep_first_error(first_err, op == MASK_SAVE      ? tcn_pointer_save()
                                        : op == MASK_DISCARD ? tcn_pointer_discard()
                                                             : tcn_pointer_restore_defaults());
    }
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_GESTURE)
    if (features & toucan_studio_Feature_FEATURE_GESTURE) {
        first_err =
            keep_first_error(first_err, op == MASK_SAVE      ? tcn_gesture_save()
                                        : op == MASK_DISCARD ? tcn_gesture_discard()
                                                             : tcn_gesture_restore_defaults());
    }
#endif
    return first_err;
}

static void handle_mask(enum mask_op op, const toucan_studio_Mask *mask,
                        toucan_studio_Response *resp) {
    if (mask->features & ~BUILT_FEATURES) {
        tcn_rpc_error(resp, toucan_studio_ErrorCode_ERROR_CODE_NOT_SUPPORTED, 1, 0);
        return;
    }

    int err = run_mask_op(op, mask->features);
    if (err) {
        LOG_WRN("toucan__studio: mask op %d on 0x%x failed: %d", op, mask->features, err);
        tcn_rpc_error(resp, tcn_rpc_errno_to_code(err), 0, 0);
        return;
    }

    fill_changes(resp);
}

/* Official "reset settings" (core.reset_settings) also resets our features to the DT defaults. */
static int toucan_studio_settings_reset(void) {
    return run_mask_op(MASK_RESTORE_DEFAULTS, BUILT_FEATURES);
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(toucan_studio, toucan_studio_settings_reset);

static bool toucan_studio_handle(const zmk_custom_CallRequest *raw_request,
                                 pb_callback_t *encode_response) {
    toucan_studio_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(toucan__studio, encode_response);

    toucan_studio_Request req = toucan_studio_Request_init_zero;
    pb_istream_t stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&stream, toucan_studio_Request_fields, &req)) {
        LOG_WRN("toucan__studio: undecodable request: %s", PB_GET_ERROR(&stream));
        tcn_rpc_error(resp, toucan_studio_ErrorCode_ERROR_CODE_INVALID, 0, 0);
        return true;
    }

    switch (req.which_op) {
    case toucan_studio_Request_get_info_tag:
        resp->which_result = toucan_studio_Response_info_tag;
        fill_info(&resp->result.info);
        break;
    case toucan_studio_Request_get_changes_tag:
        fill_changes(resp);
        break;
    case toucan_studio_Request_save_tag:
        handle_mask(MASK_SAVE, &req.op.save, resp);
        break;
    case toucan_studio_Request_discard_tag:
        handle_mask(MASK_DISCARD, &req.op.discard, resp);
        break;
    case toucan_studio_Request_restore_defaults_tag:
        handle_mask(MASK_RESTORE_DEFAULTS, &req.op.restore_defaults, resp);
        break;
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_COMBO)
    case toucan_studio_Request_list_combos_tag:
        tcn_combo_list(&req.op.list_combos, resp);
        break;
    case toucan_studio_Request_set_combo_tag:
        tcn_combo_set(&req.op.set_combo, resp);
        break;
    case toucan_studio_Request_delete_combo_tag:
        tcn_combo_delete(&req.op.delete_combo, resp);
        break;
    case toucan_studio_Request_restore_combo_tag:
        tcn_combo_restore(&req.op.restore_combo, resp);
        break;
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_MACRO)
    case toucan_studio_Request_list_macros_tag:
        tcn_macro_list(resp);
        break;
    case toucan_studio_Request_get_macro_tag:
        tcn_macro_get(&req.op.get_macro, resp);
        break;
    case toucan_studio_Request_set_macro_tag:
        tcn_macro_set_chunk(&req.op.set_macro, resp);
        break;
    case toucan_studio_Request_clear_macro_tag:
        tcn_macro_clear(&req.op.clear_macro, resp);
        break;
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_POINTER)
    case toucan_studio_Request_get_pointer_tag:
        tcn_pointer_get(resp);
        break;
    case toucan_studio_Request_set_pointer_tag:
        tcn_pointer_set(&req.op.set_pointer, resp);
        break;
    case toucan_studio_Request_get_pointer_curve_info_tag:
        tcn_pointer_curve_info(resp);
        break;
#endif
#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_GESTURE)
    case toucan_studio_Request_get_touch_gesture_settings_tag:
        tcn_gesture_get(resp);
        break;
    case toucan_studio_Request_set_touch_gesture_settings_tag:
        tcn_gesture_set(&req.op.set_touch_gesture_settings, resp);
        break;
    case toucan_studio_Request_get_touch_gesture_info_tag:
        tcn_gesture_info(resp);
        break;
#endif
    case 0:
        tcn_rpc_error(resp, toucan_studio_ErrorCode_ERROR_CODE_INVALID, 0, 0);
        break;
    default:
        tcn_rpc_error(resp, toucan_studio_ErrorCode_ERROR_CODE_NOT_SUPPORTED, 0, 0);
        break;
    }

    return true;
}
