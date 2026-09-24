/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "hardware.h"

#define PM_PASSWORD 0x5a000000u
#define PM_DATA_MASK 0x00ffffffu

static uint32_t Read(const PM_HW_IO *io, uint32_t offset)
{
    return io->Read(io->Context, offset) & PM_DATA_MASK;
}

static void Write(const PM_HW_IO *io, uint32_t offset, uint32_t value)
{
    io->Write(io->Context, offset, PM_PASSWORD | (value & PM_DATA_MASK));
}

PM_HW_RESULT PmHwAcquire(const PM_HW_IO *io, PM_HW_STATE *state)
{
    uint32_t legacy, v3d;
    if (!io || !io->Read || !io->Write || !io->DelayUs || !state || state->Unsafe)
        return PmHwInvalid;

    /* BCM2712 has a live V3D reset gate at +0x304. The legacy GRAFX
     * register at +0x10c reads like other absent PM registers on Pi 5;
     * keep its value only to detect a changed register view. */
    legacy = Read(io, PM_HW_GRAFX);
    v3d = Read(io, PM_HW_V3D);
    if (!(v3d & PM_HW_V3D_ENAB) || v3d == legacy ||
        Read(io, PM_HW_GRAFX) != legacy || Read(io, PM_HW_V3D) != v3d)
        return PmHwNotReady;

    state->LegacyParent = legacy;
    state->InitialV3d = v3d;
    state->ExpectedV3d = v3d;
    return PmHwOk;
}

PM_HW_RESULT PmHwSetV3dReset(const PM_HW_IO *io, PM_HW_STATE *state,
                            uint32_t release)
{
    uint32_t before, after, expected;
    if (!io || !io->Read || !io->Write || !io->DelayUs || !state ||
        state->Unsafe || release > 1) return PmHwInvalid;

    before = Read(io, PM_HW_V3D);
    if (Read(io, PM_HW_GRAFX) != state->LegacyParent ||
        before != state->ExpectedV3d || !(before & PM_HW_V3D_ENAB)) goto Fail;
    if (!!(before & PM_HW_V3DRSTN) == !!release) return PmHwOk;
    if (release) io->DelayUs(io->Context, 1);
    expected = (before & ~PM_HW_V3DRSTN) |
               (release ? PM_HW_V3DRSTN : 0);
    Write(io, PM_HW_V3D, expected);
    after = Read(io, PM_HW_V3D);
    if (after != expected || Read(io, PM_HW_GRAFX) != state->LegacyParent)
        goto Fail;
    state->ExpectedV3d = after;
    ++state->Transitions;
    return PmHwOk;
Fail:
    state->Unsafe = 1;
    return PmHwFault;
}

PM_HW_RESULT PmHwRelease(const PM_HW_IO *io, PM_HW_STATE *state)
{
    PM_HW_RESULT result;
    if (!io || !state || state->Unsafe) return PmHwInvalid;
    result = PmHwSetV3dReset(io, state,
                            !!(state->InitialV3d & PM_HW_V3DRSTN));
    if (result != PmHwOk) return result;
    if (Read(io, PM_HW_GRAFX) != state->LegacyParent ||
        Read(io, PM_HW_V3D) != state->InitialV3d) {
        state->Unsafe = 1;
        return PmHwFault;
    }
    return PmHwOk;
}
