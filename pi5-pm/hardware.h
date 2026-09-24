/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

#define PM_HW_GRAFX 0x10cu
#define PM_HW_V3D 0x304u
#define PM_HW_V3DRSTN 0x40u
#define PM_HW_V3D_ENAB 0x1000u

typedef struct {
    void *Context;
    uint32_t (*Read)(void *context, uint32_t offset);
    void (*Write)(void *context, uint32_t offset, uint32_t value);
    void (*DelayUs)(void *context, uint32_t microseconds);
} PM_HW_IO;

typedef struct {
    uint32_t LegacyParent, InitialV3d, ExpectedV3d;
    uint32_t Transitions, Unsafe;
} PM_HW_STATE;

typedef enum {
    PmHwOk = 0,
    PmHwNotReady = 1,
    PmHwFault = 2,
    PmHwInvalid = 3
} PM_HW_RESULT;

PM_HW_RESULT PmHwAcquire(const PM_HW_IO *io, PM_HW_STATE *state);
PM_HW_RESULT PmHwSetV3dReset(const PM_HW_IO *io, PM_HW_STATE *state,
                            uint32_t release);
PM_HW_RESULT PmHwRelease(const PM_HW_IO *io, PM_HW_STATE *state);
