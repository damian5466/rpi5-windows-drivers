/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PI5_HARDWARE_H
#define PI5_HARDWARE_H
#include <stdint.h>
#include <stddef.h>

#define PI5_KIND_RNG 1u
#define PI5_KIND_THERMAL 2u
#define PI5_RNG_BASE UINT64_C(0x107d208000)
#define PI5_RNG_SIZE 0x28u
#define PI5_THERMAL_BASE UINT64_C(0x107d542000)
#define PI5_THERMAL_SIZE 0xf00u
#define PI5_TEMP_STATUS 0x200u
#define PI5_RNG_CONTROL 0x00u
#define PI5_RNG_BIT_COUNT 0x0cu
#define PI5_RNG_STATUS 0x18u
#define PI5_RNG_FIFO_DATA 0x20u
#define PI5_RNG_FIFO_COUNT 0x24u
#define PI5_RNG_ENABLE_MASK 0x1fffu
#define PI5_RNG_FAILURE_MASK 0x80000020u
#define PI5_RNG_MAX_BYTES 4096u
#define PI5_RNG_TIMEOUT_MS 250u

enum PI5_RESULT {
    PI5_OK, PI5_INVALID, PI5_NOT_READY, PI5_TIMEOUT, PI5_HEALTH_FAILED,
    PI5_CANCELLED
};

typedef struct PI5_IO {
    void *context;
    uint32_t (*read32)(void *, uint32_t);
    uint64_t (*now_ms)(void *);
    void (*wait)(void *);
    int (*cancelled)(void *);
} PI5_IO;

typedef struct PI5_RNG_STATE {
    uint32_t last_word;
    unsigned repetitions;
    int failed;
} PI5_RNG_STATE;

int pi5_resource_valid(unsigned kind, uint64_t base, uint32_t size);
enum PI5_RESULT pi5_temperature_decode(uint32_t raw, int32_t *millicelsius);
/* No writes to the RNG registers: the UEFI driver initializes this hardware.
 * Whole-request deadline; no partial samples are returned on any failure.
 * 'failed' latches hardware faults or four identical consecutive words until
 * PnP restart. This sanity check is not an entropy/cryptographic certification.
 */
enum PI5_RESULT pi5_rng_read(const PI5_IO *io, PI5_RNG_STATE *state,
                            uint8_t *bytes, size_t length);
#endif
