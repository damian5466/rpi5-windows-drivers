/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PI5_FAN_PUBLIC_H
#define PI5_FAN_PUBLIC_H
#include <stdint.h>
/* Include Windows.h/winioctl.h or ntddk.h first. */
static const GUID GUID_DEVINTERFACE_PI5_FAN =
    {0xd8ec3564,0xb921,0x4cef,{0xaf,0xe7,0x9c,0xf7,0xd5,0x62,0x25,0x11}};
#define IOCTL_FAN_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN,0x800,METHOD_BUFFERED,FILE_READ_ACCESS)
#define IOCTL_FAN_SET CTL_CODE(FILE_DEVICE_UNKNOWN,0x801,METHOD_BUFFERED,FILE_WRITE_ACCESS)
#define FAN_ABI_VERSION 2u
typedef struct FAN_QUERY {
    uint32_t size, version;
    uint32_t percent, rpm, period_ns, duty_ticks;
    uint32_t lease_remaining_ms, expirations, failed;
    uint32_t global, control, range, phase;
    uint32_t manual, requested_percent;
    int32_t temperature_millicelsius;
    uint32_t temperature_age_ms, policy_flags, temperature_status;
} FAN_QUERY;
typedef struct FAN_SET {
    uint32_t size, version, percent, lease_ms;
} FAN_SET;
#endif
