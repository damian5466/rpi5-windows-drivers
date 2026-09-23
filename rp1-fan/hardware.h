/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PI5_FAN_HARDWARE_H
#define PI5_FAN_HARDWARE_H
#include <stdint.h>

#define FAN_MMIO_SIZE 0x100u
#define FAN_GLOBAL 0x00u
#define FAN_RPM 0x3cu
#define FAN_CONTROL 0x44u
#define FAN_RANGE 0x48u
#define FAN_PHASE 0x4cu
#define FAN_DUTY 0x50u
#define FAN_TICKS 2078u
#define FAN_PERIOD_NS 41560u
#define FAN_MAX_LEASE_MS 5000u
#define FAN_TEMP_MAX_AGE_MS 2500u
#define FAN_SPINUP_MS 2000u
#define FAN_POLICY_SPINUP 1u
#define FAN_POLICY_NO_TEMPERATURE 2u
#define FAN_POLICY_STALE_TEMPERATURE 4u
#define FAN_POLICY_STALLED 8u
#define FAN_POLICY_HOT 16u

enum FAN_RESULT { FAN_OK, FAN_INVALID, FAN_NOT_READY, FAN_IO_ERROR };
typedef struct FAN_IO {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
} FAN_IO;
typedef struct FAN_STATE {
    uint64_t deadline;
    uint32_t percent;
    uint32_t expirations;
    uint32_t failed;
} FAN_STATE;
typedef struct FAN_POLICY {
    uint64_t full_until, temperature_time, next_rpm;
    int32_t temperature;
    uint32_t stalls, previous, flags;
} FAN_POLICY;

int fan_resource_valid(uint64_t address, uint32_t length);
enum FAN_RESULT fan_ready(const FAN_IO *io);
enum FAN_RESULT fan_full(const FAN_IO *io, FAN_STATE *state);
enum FAN_RESULT fan_adjust(const FAN_IO *io, FAN_STATE *state, uint32_t percent);
enum FAN_RESULT fan_set(const FAN_IO *io, FAN_STATE *state, uint32_t percent,
                        uint32_t lease_ms, uint64_t now);
enum FAN_RESULT fan_tick(const FAN_IO *io, FAN_STATE *state, uint64_t now);
uint32_t fan_curve(int32_t temperature, uint32_t previous);
void fan_policy_start(FAN_POLICY *policy, uint64_t now);
void fan_policy_full(FAN_POLICY *policy, uint64_t now);
void fan_policy_temperature(FAN_POLICY *policy, int32_t temperature, uint64_t now);
uint32_t fan_policy_target(FAN_POLICY *policy, uint64_t now, uint32_t rpm,
                          int manual, uint32_t requested);
#endif
