/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "hardware.h"

int pi5_resource_valid(unsigned kind, uint64_t base, uint32_t size)
{
    return (kind == PI5_KIND_RNG && base == PI5_RNG_BASE && size == PI5_RNG_SIZE) ||
           (kind == PI5_KIND_THERMAL && base == PI5_THERMAL_BASE && size == PI5_THERMAL_SIZE);
}

enum PI5_RESULT pi5_temperature_decode(uint32_t raw, int32_t *millicelsius)
{
    int32_t temperature;
    if (!millicelsius) return PI5_INVALID;
    if (raw == UINT32_MAX || !(raw & 0x10400u)) return PI5_NOT_READY;
    /* BCM2712 DT coefficients, not the different BCM2711 calibration. */
    temperature = 450000 - 550 * (int32_t)(raw & 0x3ffu);
    if (temperature < -40000 || temperature > 125000) return PI5_NOT_READY;
    *millicelsius = temperature;
    return PI5_OK;
}

enum PI5_RESULT pi5_rng_read(const PI5_IO *io, PI5_RNG_STATE *state,
                            uint8_t *bytes, size_t length)
{
    uint64_t start;
    size_t done = 0, index;
    enum PI5_RESULT result = PI5_OK;
    if (!io || !state || !bytes || !length || length > PI5_RNG_MAX_BYTES ||
        !io->read32 || !io->now_ms || !io->wait || !io->cancelled) return PI5_INVALID;
    start = io->now_ms(io->context);
    while (done < length) {
        uint32_t control, status, count, bits, word;
        unsigned i;
        if (io->cancelled(io->context)) { result = PI5_CANCELLED; break; }
        if (state->failed) { result = PI5_HEALTH_FAILED; break; }
        if (io->now_ms(io->context) - start >= PI5_RNG_TIMEOUT_MS) {
            result = PI5_TIMEOUT;
            break;
        }
        control = io->read32(io->context, PI5_RNG_CONTROL);
        status = io->read32(io->context, PI5_RNG_STATUS);
        if (control == UINT32_MAX || status == UINT32_MAX) {
            result = PI5_NOT_READY;
            break;
        }
        if (status & PI5_RNG_FAILURE_MASK) {
            state->failed = 1;
            result = PI5_HEALTH_FAILED;
            break;
        }
        if (!(control & PI5_RNG_ENABLE_MASK)) { result = PI5_NOT_READY; break; }
        bits = io->read32(io->context, PI5_RNG_BIT_COUNT);
        count = io->read32(io->context, PI5_RNG_FIFO_COUNT);
        if (count == UINT32_MAX) { result = PI5_NOT_READY; break; }
        if (bits <= 16 || !(count & 0xffu)) {
            io->wait(io->context);
            continue;
        }
        word = io->read32(io->context, PI5_RNG_FIFO_DATA);
        state->repetitions = (state->repetitions && state->last_word == word) ?
                            state->repetitions + 1 : 1;
        state->last_word = word;
        if (state->repetitions >= 4) {
            state->failed = 1;
            result = PI5_HEALTH_FAILED;
            break;
        }
        for (i = 0; i < 4 && done < length; ++i, ++done)
            bytes[done] = (uint8_t)(word >> (i * 8));
    }
    /* A fault raised during the final FIFO read also invalidates the sample. */
    if (result == PI5_OK) {
        uint32_t status = io->read32(io->context, PI5_RNG_STATUS);
        if (status == UINT32_MAX) result = PI5_NOT_READY;
        else if (status & PI5_RNG_FAILURE_MASK) {
            state->failed = 1;
            result = PI5_HEALTH_FAILED;
        }
    }
    if (result != PI5_OK)
        for (index = 0; index < length; ++index) bytes[index] = 0;
    return result;
}
