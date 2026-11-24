/*
 * Devicetree constants shared by both Toucan halves for the touch gestures.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/*
 * EV_ABS code of the right half's auxiliary touch coordinate (cirque-input-module
 * aux-abs-code): value = x | (y << 11), each axis scaled to 0..1024. Reserved for Toucan;
 * no standard ABS code and no ZMK listener uses it.
 */
#define TOUCAN_AUX_ABS_CODE 0x7F00

/* &tgs (touch gesture on/off key) parameter. */
#define TGS_TOG 0
#define TGS_ON 1
#define TGS_OFF 2
