/* Placeholder until the runtime pointer feature lands (replaced in Wave C). */
#include <toucan/studio_rpc.h>

#define TCN_NOT_SUPPORTED(resp) tcn_rpc_error(resp, toucan_studio_ErrorCode_ERROR_CODE_NOT_SUPPORTED, 0, 0)

void tcn_pointer_info(toucan_studio_PointerInfo *info) { (void)info; }
void tcn_pointer_get(toucan_studio_Response *resp) { TCN_NOT_SUPPORTED(resp); }
void tcn_pointer_set(const toucan_studio_SetPointerRequest *req, toucan_studio_Response *resp) { (void)req; TCN_NOT_SUPPORTED(resp); }
bool tcn_pointer_unsaved(void) { return false; }
int tcn_pointer_save(void) { return 0; }
int tcn_pointer_discard(void) { return 0; }
int tcn_pointer_restore_defaults(void) { return 0; }
