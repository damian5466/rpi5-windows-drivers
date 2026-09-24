/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "mailbox.h"
#include <string.h>

#define RX_DATA 0x00u
#define RX_STATUS 0x18u
#define TX_DATA 0x20u
#define TX_STATUS 0x38u
#define FIFO_FULL 0x80000000u
#define FIFO_EMPTY 0x40000000u

static MBX_RESULT Fail(MBX_TRANSPORT *t, MBX_RESULT result, int fatal)
{
    ++t->Failures;
    if (fatal) t->Fault = 1;
    if (result == MbxTimeout) ++t->Timeouts;
    if (result == MbxForeignReply) ++t->ForeignReplies;
    return result;
}

int MbxEncodeAddress(uint64_t logical, uint64_t physical, uint32_t bytes, uint32_t *encoded)
{
    if (!bytes || bytes > 0x40000000u || (logical & 15) || (physical & 15) ||
        physical > 0x40000000ull - bytes) return 0;
    if (logical == physical + 0xc0000000ull) *encoded = (uint32_t)logical;
    else if (logical == physical) *encoded = (uint32_t)logical + 0xc0000000u;
    else return 0; /* No guessing for a different DMA translation or IOMMU. */
    return 1;
}

MBX_RESULT MbxTransfer(MBX_TRANSPORT *t, uint32_t tag, uint32_t *data, uint32_t bytes)
{
    uint64_t deadline;
    uint32_t size, message, reply, i;
    if (!t || !t->Buffer || !data || !tag || !bytes || (bytes & 3) ||
        bytes > MBX_BUFFER_BYTES - 24 || (t->Address & 15) ||
        t->Address < 0xc0000000u || t->Address > UINT32_MAX - MBX_BUFFER_BYTES + 1)
        return MbxInvalid;
    if (t->Fault || t->Unsafe) return MbxFaulted;
    deadline = t->Io.Now(t->Io.Context) + MBX_TIMEOUT_100NS;
    /* A clean handoff has no outstanding firmware messages. Never consume
     * another owner's reply and pretend that ownership was exclusive. */
    if (!(t->Io.Read(t->Io.Context, RX_STATUS) & FIFO_EMPTY))
        return Fail(t, MbxForeignReply, 1);
    while (t->Io.Read(t->Io.Context, TX_STATUS) & FIFO_FULL) {
        if (t->Io.Now(t->Io.Context) >= deadline) return Fail(t, MbxTimeout, 1);
        t->Io.Pause(t->Io.Context);
    }
    size = bytes + 24;
    /* _CCA=0 common buffers are Device Memory on Windows ARM64. Use aligned
     * volatile words: memcpy/memset may emit unaligned vector accesses. */
    for (i = 0; i < MBX_BUFFER_BYTES / 4; ++i) t->Buffer[i] = 0;
    t->Buffer[0] = size;
    t->Buffer[2] = tag;
    t->Buffer[3] = bytes;
    for (i = 0; i < bytes / 4; ++i) t->Buffer[5 + i] = data[i];
    message = t->Address | 8u;
    t->Io.Barrier(t->Io.Context);
    t->Unsafe = 1;
    ++t->Transactions;
    t->Io.Write(t->Io.Context, TX_DATA, message);
    for (;;) {
        if (!(t->Io.Read(t->Io.Context, RX_STATUS) & FIFO_EMPTY)) {
            reply = t->Io.Read(t->Io.Context, RX_DATA);
            t->LastReply = reply;
            if (reply != message) return Fail(t, MbxForeignReply, 1);
            t->Io.Barrier(t->Io.Context);
            t->Unsafe = 0;
            break;
        }
        if (t->Io.Now(t->Io.Context) >= deadline) return Fail(t, MbxTimeout, 1);
        t->Io.Pause(t->Io.Context);
    }
    if (t->Buffer[0] != size || t->Buffer[2] != tag || t->Buffer[3] != bytes ||
        t->Buffer[size / 4 - 1] != 0)
        return Fail(t, MbxMalformed, 1);
    if (t->Buffer[1] == 0x80000001u) return Fail(t, MbxFirmwareError, 0);
    /* The Pi 5 RTC tags count only the four-byte register value in their
     * response length; the selector and value still occupy eight bytes.
     * Accept that exact encoding as well as the full payload length. */
    if (t->Buffer[1] != 0x80000000u ||
        (t->Buffer[4] != (0x80000000u | bytes) &&
         !((tag == 0x30087u || tag == 0x38087u) && bytes == 8 && t->Buffer[4] == 0x80000004u) &&
         !(tag == 0x38002u && bytes == 12 && t->Buffer[4] == 0x80000008u)))
        return Fail(t, MbxMalformed, 1);
    for (i = 0; i < bytes / 4; ++i) data[i] = t->Buffer[5 + i];
    return MbxOk;
}

static int Leap(uint32_t year)
{
    return !(year % 4) && ((year % 100) || !(year % 400));
}

static uint32_t MonthDays(uint32_t year, uint32_t month)
{
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month - 1] + (uint32_t)(month == 2 && Leap(year));
}

static uint16_t Word(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

int MbxTimeToEpoch(const uint8_t time[16], uint32_t *epoch)
{
    uint32_t year = Word(time), month = time[2], day = time[3], y, m;
    int32_t zone = (int16_t)Word(time + 10);
    int64_t days = 0, seconds;
    if (year < 1900 || year > 9999 || month < 1 || month > 12 ||
        day < 1 || day > MonthDays(year, month) || time[4] > 23 ||
        time[5] > 59 || time[6] > 59 || Word(time + 8) > 1000 ||
        (zone != 2047 && (zone < -1440 || zone > 1440)) ||
        (time[12] & ~3u) || time[13] || time[14] || time[15]) return 0;
    if (year >= 1970) {
        for (y = 1970; y < year; ++y) days += 365 + Leap(y);
    } else {
        for (y = year; y < 1970; ++y) days -= 365 + Leap(y);
    }
    for (m = 1; m < month; ++m) days += MonthDays(year, m);
    days += day - 1;
    seconds = days * 86400 + time[4] * 3600 + time[5] * 60 + time[6];
    /* Same UTC policy as RpiRtcLib: unspecified means UTC. Daylight describes
     * the supplied local time; it is not an additional one-hour correction. */
    if (zone != 2047) seconds -= (int64_t)zone * 60;
    if (seconds < 0 || seconds > UINT32_MAX) return 0;
    *epoch = (uint32_t)seconds;
    return 1;
}

void MbxEpochToTime(uint32_t epoch, uint8_t time[16])
{
    uint32_t days = epoch / 86400, seconds = epoch % 86400;
    uint32_t year = 1970, month = 1, n;
    while (days >= (n = 365u + (uint32_t)Leap(year))) { days -= n; ++year; }
    while (days >= (n = MonthDays(year, month))) { days -= n; ++month; }
    memset(time, 0, 16);
    time[0] = (uint8_t)year; time[1] = (uint8_t)(year >> 8);
    time[2] = (uint8_t)month; time[3] = (uint8_t)(days + 1);
    time[4] = (uint8_t)(seconds / 3600);
    time[5] = (uint8_t)((seconds / 60) % 60);
    time[6] = (uint8_t)(seconds % 60);
    time[7] = 1; /* Valid, second precision, UTC. */
}
