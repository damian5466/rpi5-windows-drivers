/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MBX_BUFFER_BYTES 64u
#define MBX_TIMEOUT_100NS 30000000ull
typedef enum {
    MbxOk, MbxInvalid, MbxTimeout, MbxForeignReply, MbxMalformed,
    MbxFirmwareError, MbxFaulted
} MBX_RESULT;
typedef struct {
    void *Context;
    uint32_t (*Read)(void *, uint32_t);
    void (*Write)(void *, uint32_t, uint32_t);
    uint64_t (*Now)(void *);
    void (*Pause)(void *);
    void (*Barrier)(void *);
} MBX_IO;
typedef struct {
    MBX_IO Io;
    volatile uint32_t *Buffer;
    uint32_t Address;
    uint32_t Fault, Unsafe, Transactions, Failures, Timeouts, ForeignReplies;
    uint32_t LastReply;
} MBX_TRANSPORT;

/* Caller owns serialization and a HAL common buffer. A submitted buffer may
 * only be reused/freed after the matching reply. Fault is sticky until reboot. */
MBX_RESULT MbxTransfer(MBX_TRANSPORT *t, uint32_t tag, uint32_t *data, uint32_t bytes);
int MbxEncodeAddress(uint64_t logical, uint64_t physical, uint32_t bytes, uint32_t *encoded);
int MbxTimeToEpoch(const uint8_t time[16], uint32_t *epoch);
void MbxEpochToTime(uint32_t epoch, uint8_t time[16]);
