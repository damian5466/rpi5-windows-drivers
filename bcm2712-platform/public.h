/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PI5_PLATFORM_PUBLIC_H
#define PI5_PLATFORM_PUBLIC_H
#include <stdint.h>

/* Include Windows.h/winioctl.h or ntddk.h before this header. */
static const GUID GUID_DEVINTERFACE_PI5_PLATFORM =
    {0xaf0731ab,0x69f1,0x46d6,{0x98,0xe9,0xa5,0x73,0xc4,0x26,0x61,0xe2}};
#define IOCTL_PI5_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN,0x800,METHOD_BUFFERED,FILE_READ_ACCESS)
#define IOCTL_PI5_RANDOM CTL_CODE(FILE_DEVICE_UNKNOWN,0x801,METHOD_BUFFERED,FILE_READ_ACCESS)
#define PI5_ABI_VERSION 1u
typedef struct PI5_QUERY {
    uint32_t size;
    uint32_t version;
    uint32_t kind;
    int32_t temperature_millicelsius;
    uint32_t temperature_raw;
    uint32_t rng_control;
    uint32_t rng_status;
    uint32_t rng_bit_count;
    uint32_t rng_fifo_count;
    uint32_t rng_failed;
} PI5_QUERY;
#endif
