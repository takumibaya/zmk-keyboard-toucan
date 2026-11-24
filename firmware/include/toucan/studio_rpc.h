/*
 * Shared helpers for the toucan__studio custom RPC handlers.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include <toucan/studio.pb.h>

#define TOUCAN_STUDIO_PROTOCOL_VERSION 1

/* Fill resp with an Error result. */
void tcn_rpc_error(toucan_studio_Response *resp, toucan_studio_ErrorCode code, uint8_t field,
                   uint8_t index);
