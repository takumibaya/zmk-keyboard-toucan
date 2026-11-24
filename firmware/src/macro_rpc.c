/* Placeholder until the runtime macro feature lands (replaced in Wave C). */
#include <toucan/studio_rpc.h>

#define TCN_NOT_SUPPORTED(resp) tcn_rpc_error(resp, toucan_studio_ErrorCode_ERROR_CODE_NOT_SUPPORTED, 0, 0)

void tcn_macro_info(toucan_studio_MacroInfo *info) { (void)info; }
void tcn_macro_list(toucan_studio_Response *resp) { TCN_NOT_SUPPORTED(resp); }
void tcn_macro_get(const toucan_studio_GetMacroRequest *req, toucan_studio_Response *resp) { (void)req; TCN_NOT_SUPPORTED(resp); }
void tcn_macro_set_chunk(const toucan_studio_SetMacroChunkRequest *req, toucan_studio_Response *resp) { (void)req; TCN_NOT_SUPPORTED(resp); }
void tcn_macro_clear(const toucan_studio_SlotRequest *req, toucan_studio_Response *resp) { (void)req; TCN_NOT_SUPPORTED(resp); }
bool tcn_macro_unsaved(void) { return false; }
int tcn_macro_save(void) { return 0; }
int tcn_macro_discard(void) { return 0; }
int tcn_macro_restore_defaults(void) { return 0; }
