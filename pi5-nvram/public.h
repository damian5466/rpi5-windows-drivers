/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PI5_NVRAM_PUBLIC_H
#define PI5_NVRAM_PUBLIC_H
#define NV_DRIVER_VERSION 1U
#define IOCTL_NV_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN,0x801,METHOD_BUFFERED,FILE_READ_DATA)
#define IOCTL_NV_FLUSH CTL_CODE(FILE_DEVICE_UNKNOWN,0x802,METHOD_BUFFERED,FILE_WRITE_DATA)
typedef struct {
    unsigned int size, version, active, matching_volumes;
    unsigned long long current_sequence, saved_sequence, saves, failures;
    unsigned int last_status, stopped;
    WCHAR volume[512];
} NV_DRIVER_QUERY;
#endif
