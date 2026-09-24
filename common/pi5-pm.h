/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

#define PI5_PM_NAME L"\\Device\\Pi5Pm"
#define PI5_PM_PATH L"\\\\.\\Pi5Pm"
#define PI5_PM_VERSION 1u
#define PI5_PM_V3D_DOMAIN 1u
#define PI5_PM_V3D_RESET 0u

#define IOCTL_PI5_PM_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x880, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_PI5_PM_ACQUIRE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x881, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_PM_TRANSITION CTL_CODE(FILE_DEVICE_UNKNOWN, 0x882, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_PI5_PM_RELEASE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x883, METHOD_BUFFERED, FILE_WRITE_DATA)

typedef struct {
    uint32_t Version, Domain, Reset, Reserved;
} PI5_PM_LEASE_REQUEST;

enum {
    Pi5PmV3dReleaseReset = 1,
    Pi5PmV3dAssertReset = 2,
    Pi5PmV3dPulseReset = 3
};
typedef struct {
    uint32_t Version, Domain, Operation, Reserved;
} PI5_PM_TRANSITION;

/* Diagnostic values are snapshots. ParentRaw is the opaque legacy PM+0x10c
 * readback; this provider never writes it. V3dRaw is the BCM2712 gate at
 * PM+0x304, with bit 12 indicating an available gate and bit 6 deasserting
 * V3D reset. ParentUntouched is always 1. The firmware clock lease is a
 * separate dependency; V3D 7.1 SMS power management belongs to the GPU
 * consumer and is outside this reset-gate service. */
typedef struct {
    uint32_t Version, Online, Owner, Unsafe;
    uint32_t ParentRaw, V3dRaw, InitialV3dRaw, ExpectedV3dRaw;
    uint32_t GateAvailable, Transitions, Failures, LastStatus;
    uint32_t LastOperation, Debug, ParentUntouched, Reserved;
    uint32_t RstcRaw, RstsRaw, WdogRaw;
    uint32_t ImageRaw, HdmiRaw, UsbRaw;
} PI5_PM_STATUS;
