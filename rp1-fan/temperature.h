/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PI5_FAN_TEMPERATURE_H
#define PI5_FAN_TEMPERATURE_H
#include <stddef.h>
#include <stdint.h>

/* Parse the standard MSAcpi_ThermalZoneTemperature WNODE_ALL_DATA chain.
 * On success, return the hottest valid instance, independent of its provider.
 * Empty, malformed or entirely invalid readings return 0 and INT32_MIN. */
int fan_temperature_parse(const void *buffer, size_t length, int32_t *temperature);
#endif
