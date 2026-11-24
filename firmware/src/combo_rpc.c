/* Placeholder until the runtime combo feature lands (replaced in Wave C). */
#include <toucan/studio_rpc.h>

#define TCN_NOT_SUPPORTED(resp) tcn_rpc_error(resp, toucan_studio_ErrorCode_ERROR_CODE_NOT_SUPPORTED, 0, 0)

void tcn_combo_info(toucan_studio_ComboInfo *info) { (void)info; }
void tcn_combo_list(const toucan_studio_ListCombosRequest *req, toucan_studio_Response *resp) { (void)req; TCN_NOT_SUPPORTED(resp); }
void tcn_combo_set(const toucan_studio_SetComboRequest *req, toucan_studio_Response *resp) { (void)req; TCN_NOT_SUPPORTED(resp); }
void tcn_combo_delete(const toucan_studio_SlotRequest *req, toucan_studio_Response *resp) { (void)req; TCN_NOT_SUPPORTED(resp); }
void tcn_combo_restore(const toucan_studio_SlotRequest *req, toucan_studio_Response *resp) { (void)req; TCN_NOT_SUPPORTED(resp); }
bool tcn_combo_unsaved(void) { return false; }
int tcn_combo_save(void) { return 0; }
int tcn_combo_discard(void) { return 0; }
int tcn_combo_restore_defaults(void) { return 0; }
