/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "hardware.h"

int fan_resource_valid(uint64_t address, uint32_t length)
{
    /* RP1 BAR1 is 4 MiB aligned, inside the platform's PCIe outbound aperture.
     * Only its PWM1 registers are accepted, including after BAR relocation. */
    return length == FAN_MMIO_SIZE && address >= UINT64_C(0x1f00000000) &&
           address < UINT64_C(0x2000000000) &&
           (address & UINT64_C(0x3fffff)) == 0x9c000;
}

enum FAN_RESULT fan_ready(const FAN_IO *io)
{
    uint32_t global = io->read(io->context, FAN_GLOBAL);
    /* RPI00F1 is the firmware's positive detection/clock/pin handoff contract.
     * Recheck its register shape on every write. Never initialize absent fans
     * or take over other channels, clocks, GPIO, DMA or interrupts here. */
    if ((global & ~UINT32_C(0x80000000)) != 8 ||
        io->read(io->context, FAN_CONTROL) != 0x109 ||
        io->read(io->context, FAN_RANGE) != FAN_TICKS ||
        io->read(io->context, FAN_PHASE) != 0 ||
        io->read(io->context, FAN_DUTY) > FAN_TICKS)
        return FAN_NOT_READY;
    return FAN_OK;
}

static enum FAN_RESULT apply(const FAN_IO *io, uint32_t percent)
{
    uint32_t ticks = (FAN_TICKS * percent + 50) / 100;
    enum FAN_RESULT result = fan_ready(io);
    if (result != FAN_OK) return result;
    /* DUTY has its own update strobe, latching on the next channel overflow.
     * No global UPDATE write is needed; preserve firmware tachometer state. */
    io->write(io->context, FAN_DUTY, ticks);
    return io->read(io->context, FAN_DUTY) == ticks ? FAN_OK : FAN_IO_ERROR;
}

enum FAN_RESULT fan_full(const FAN_IO *io, FAN_STATE *state)
{
    enum FAN_RESULT result = apply(io, 100);
    state->deadline = 0;
    if (result == FAN_OK) state->percent = 100;
    else state->failed = 1;
    return result;
}

enum FAN_RESULT fan_set(const FAN_IO *io, FAN_STATE *state, uint32_t percent,
                        uint32_t lease_ms, uint64_t now)
{
    enum FAN_RESULT result;
    if (percent > 100 || lease_ms < 500 || lease_ms > FAN_MAX_LEASE_MS ||
        now > UINT64_MAX - lease_ms) return FAN_INVALID;
    result = fan_adjust(io, state, percent);
    if (result == FAN_OK) state->deadline = now + lease_ms;
    return result;
}

enum FAN_RESULT fan_adjust(const FAN_IO *io, FAN_STATE *state, uint32_t percent)
{
    enum FAN_RESULT result;
    if (percent > 100) return FAN_INVALID;
    if (state->failed) return FAN_IO_ERROR;
    result = apply(io, percent);
    if (result != FAN_OK) {
        state->failed = 1;
        (void)fan_full(io, state);
        return result;
    }
    state->percent = percent;
    return FAN_OK;
}

enum FAN_RESULT fan_tick(const FAN_IO *io, FAN_STATE *state, uint64_t now)
{
    if (!state->deadline || now < state->deadline) return FAN_OK;
    ++state->expirations;
    return fan_full(io, state);
}

uint32_t fan_curve(int32_t temperature, uint32_t previous)
{
    /* Always maintain airflow. Rise immediately, fall with 3 C hysteresis. */
    if (temperature < -40000 || temperature > 125000) return 100;
    if (temperature >= 75000 || (previous == 100 && temperature >= 72000)) return 100;
    if (temperature >= 65000 || (previous >= 70 && temperature >= 62000)) return 70;
    if (temperature >= 55000 || (previous >= 50 && temperature >= 52000)) return 50;
    return 35;
}

void fan_policy_full(FAN_POLICY *policy, uint64_t now)
{
    policy->full_until = now > UINT64_MAX - FAN_SPINUP_MS ? UINT64_MAX : now + FAN_SPINUP_MS;
    policy->previous = 100;
}

void fan_policy_start(FAN_POLICY *policy, uint64_t now)
{
    policy->temperature = INT32_MIN;
    policy->temperature_time = 0;
    policy->next_rpm = 0;
    policy->stalls = 0;
    policy->flags = FAN_POLICY_SPINUP | FAN_POLICY_NO_TEMPERATURE;
    fan_policy_full(policy, now);
}

void fan_policy_temperature(FAN_POLICY *policy, int32_t temperature, uint64_t now)
{
    policy->temperature = temperature;
    policy->temperature_time = now;
}

uint32_t fan_policy_target(FAN_POLICY *policy, uint64_t now, uint32_t rpm,
                          int manual, uint32_t requested)
{
    uint32_t target;
    policy->flags = 0;
    if (now >= policy->next_rpm) {
        if (!rpm || rpm > 30000) { if (policy->stalls < 3) ++policy->stalls; }
        else policy->stalls = 0;
        policy->next_rpm = now > UINT64_MAX - 1000 ? UINT64_MAX : now + 1000;
    }
    if (now < policy->full_until) policy->flags |= FAN_POLICY_SPINUP;
    if (policy->temperature < -40000 || policy->temperature > 125000)
        policy->flags |= FAN_POLICY_NO_TEMPERATURE;
    if (now < policy->temperature_time || now - policy->temperature_time > FAN_TEMP_MAX_AGE_MS)
        policy->flags |= FAN_POLICY_STALE_TEMPERATURE;
    if (policy->stalls >= 3) policy->flags |= FAN_POLICY_STALLED;
    if (policy->temperature >= 75000) policy->flags |= FAN_POLICY_HOT;
    target = manual ? requested : fan_curve(policy->temperature, policy->previous);
    if (policy->flags || target > 100) target = 100;
    if (!manual) policy->previous = target;
    return target;
}
