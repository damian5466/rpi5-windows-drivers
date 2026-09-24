/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

#define PI5_FCLK_NAME L"\\Device\\Pi5Fclk"
#define PI5_FCLK_PATH L"\\\\.\\Pi5Fclk"
#define PI5_FCLK_VERSION 1u

/* The mutating IOCTLs require a kernel requestor and a lease on this file. */
#define IOCTL_PI5_FCLK_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x860, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_PI5_FCLK_ACQUIRE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x861, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_PI5_FCLK_RELEASE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x862, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_PI5_FCLK_SET_RATE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x863, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_PI5_FCLK_SET_STATE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x864, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define PI5_FCLK_CORE 4u
#define PI5_FCLK_V3D 5u
#define PI5_FCLK_M2MC 13u
#define PI5_FCLK_PIXEL_BVB 14u
#define PI5_FCLK_DISP 16u

#define PI5_FCLK_FLAG_READ_ONLY 0x1u
#define PI5_FCLK_FLAG_MUTABLE 0x2u
#define PI5_FCLK_FLAG_UNCERTAIN 0x4u
#define PI5_FCLK_FLAG_STATE_REQUESTED 0x8u

typedef struct {
    uint32_t Version, Id, Value, Reserved;
} PI5_FCLK_REQUEST;
typedef struct {
    uint32_t Version, Id, State, RateHz, MinHz, MaxHz, Users, Flags;
} PI5_FCLK_STATUS;

/* QUERY is accessible to administrators and SYSTEM. ACQUIRE and RELEASE use
 * Value=0. One file may lease several IDs; each lease is idempotent on that
 * file and lasts until RELEASE or file cleanup. Only a sole V3D lease owner
 * may set its rate/state. The owner must quiesce V3D before explicit RELEASE;
 * closing a modified V3D lease without RELEASE latches uncertainty instead
 * of reclocking hardware that may still have work in flight. State is the
 * firmware's raw GET_CLOCK_STATE result (0 or 1), not an electrical clock
 * guarantee. STATE_REQUESTED means the current V3D lease received a matching
 * successful SET_CLOCK_STATE(1) reply; the raw State can still be 0. Rate is
 * in hertz. */
