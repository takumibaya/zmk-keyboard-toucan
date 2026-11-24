/*
 * Custom Studio RPC subsystem "toucan__studio" (see proto/toucan/studio.proto).
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <pb_decode.h>

#include <zmk/studio/custom.h>

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

static void fill_info(toucan_studio_Info *info) {
    *info = (toucan_studio_Info)toucan_studio_Info_init_zero;
    info->protocol_version = TOUCAN_STUDIO_PROTOCOL_VERSION;
    info->features = toucan_studio_Feature_FEATURE_NONE;
}

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
    default:
        tcn_rpc_error(resp, toucan_studio_ErrorCode_ERROR_CODE_NOT_SUPPORTED, 0, 0);
        break;
    }

    return true;
}
