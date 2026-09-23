// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// Register positions, not Linux GPIO numbers: D0 retains GPIO numbering but
// removes pins and compacts the mux/pull fields. AON pin 3 belongs to SD voltage.
typedef struct { uint16_t MuxBit, PullBit; } BCM_PIN_FIELDS;
int BcmPinFields(unsigned Revision, unsigned Aon, unsigned Pin, BCM_PIN_FIELDS *Fields);
uint64_t BcmValidPins(unsigned Revision, unsigned Aon);
