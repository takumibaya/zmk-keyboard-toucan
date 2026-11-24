/*
 * Shared declarations for the toucan__studio custom RPC (proto/toucan/studio.proto).
 *
 * studio_rpc.c decodes the request and calls the feature hooks below. Hooks run
 * on the Studio RPC thread, one request at a time. A request hook fills `resp`
 * completely: either its success result or an Error via tcn_rpc_error().
 * Save / discard / restore hooks return 0 or a negative errno that the
 * dispatcher maps to an ErrorCode:
 *   -EBUSY -> BUSY, -ENOENT -> NOT_FOUND, -ENOTSUP -> NOT_SUPPORTED,
 *   -ERANGE -> OUT_OF_RANGE, -EINVAL -> INVALID, anything else -> STORAGE.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <toucan/studio.pb.h>

#define TOUCAN_STUDIO_PROTOCOL_VERSION 3

/* Settings subtrees (Zephyr settings) owned by the features. */
#define TOUCAN_SETTINGS_COMBO "tcn/cmb"   /* tcn/cmb/<slot> */
#define TOUCAN_SETTINGS_MACRO "tcn/mac"   /* tcn/mac/<slot> */
#define TOUCAN_SETTINGS_POINTER "tcn/ptr" /* single record */
#define TOUCAN_SETTINGS_GESTURE "tcn/gst" /* single record; the on/off key uses "tcn/gsw" */

/* Fill resp with an Error result. */
void tcn_rpc_error(toucan_studio_Response *resp, toucan_studio_ErrorCode code, uint8_t field,
                   uint8_t index);

/* Map a negative errno to an ErrorCode (see the table above). */
toucan_studio_ErrorCode tcn_rpc_errno_to_code(int err);

#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_COMBO)
void tcn_combo_info(toucan_studio_ComboInfo *info);
void tcn_combo_list(const toucan_studio_ListCombosRequest *req, toucan_studio_Response *resp);
void tcn_combo_set(const toucan_studio_SetComboRequest *req, toucan_studio_Response *resp);
void tcn_combo_delete(const toucan_studio_SlotRequest *req, toucan_studio_Response *resp);
void tcn_combo_restore(const toucan_studio_SlotRequest *req, toucan_studio_Response *resp);
bool tcn_combo_unsaved(void);
int tcn_combo_save(void);
int tcn_combo_discard(void);
int tcn_combo_restore_defaults(void); /* RAM back to the DT combos and stored overrides erased */
#endif

#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_MACRO)
void tcn_macro_info(toucan_studio_MacroInfo *info);
void tcn_macro_list(toucan_studio_Response *resp);
void tcn_macro_get(const toucan_studio_GetMacroRequest *req, toucan_studio_Response *resp);
void tcn_macro_set_chunk(const toucan_studio_SetMacroChunkRequest *req,
                         toucan_studio_Response *resp);
void tcn_macro_clear(const toucan_studio_SlotRequest *req, toucan_studio_Response *resp);
bool tcn_macro_unsaved(void);
int tcn_macro_save(void);
int tcn_macro_discard(void);
int tcn_macro_restore_defaults(void); /* all slots empty and stored macros erased */
#endif

#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_POINTER)
void tcn_pointer_info(toucan_studio_PointerInfo *info);
void tcn_pointer_curve_info(toucan_studio_Response *resp);
void tcn_pointer_get(toucan_studio_Response *resp);
void tcn_pointer_set(const toucan_studio_SetPointerRequest *req, toucan_studio_Response *resp);
bool tcn_pointer_unsaved(void);
int tcn_pointer_save(void);
int tcn_pointer_discard(void);
int tcn_pointer_restore_defaults(void); /* DT values in RAM and the stored record erased */
#endif

#if IS_ENABLED(CONFIG_TOUCAN_STUDIO_GESTURE)
void tcn_gesture_info(toucan_studio_Response *resp);
void tcn_gesture_get(toucan_studio_Response *resp);
void tcn_gesture_set(const toucan_studio_SetTouchGestureRequest *req, toucan_studio_Response *resp);
bool tcn_gesture_unsaved(void);
int tcn_gesture_save(void);
int tcn_gesture_discard(void);
int tcn_gesture_restore_defaults(void); /* DT values in RAM, stored record erased, key back on */
#endif
