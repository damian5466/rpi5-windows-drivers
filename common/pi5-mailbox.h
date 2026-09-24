/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

#define PI5_MAILBOX_NAME L"\\Device\\Pi5Mailbox"
#define PI5_MAILBOX_PATH L"\\\\.\\Pi5Mailbox"
#define PI5_MAILBOX_VERSION 1u
#define IOCTL_PI5_MAILBOX_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x850, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_PI5_MAILBOX_STATS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x851, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_PI5_MAILBOX_CLOCK_CONTROL CTL_CODE(FILE_DEVICE_UNKNOWN, 0x852, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

/* Fixed queries. There is no raw tag or physical-address passthrough. */
enum {
    Pi5MailboxFirmwareRevision = 1,
    Pi5MailboxClockRate = 2,
    Pi5MailboxClockState = 3,
    Pi5MailboxRtcSeconds = 4,
    Pi5MailboxClockMinRate = 5,
    Pi5MailboxClockMaxRate = 6
};
/* Kernel-only operations, exclusively on V3D clock 5. A controller must claim
 * its file before writing, restore the queried baseline, then release it.
 * Closing an unreleased claim prevents another controller until reboot;
 * unrelated read-only and RTC traffic can continue. */
enum {
    Pi5MailboxClockClaim = 1,
    Pi5MailboxClockRelease = 2,
    Pi5MailboxClockSetRate = 3,
    Pi5MailboxClockSetState = 4
};
typedef struct {
    uint32_t Version, Operation, Id, Value;
} PI5_MAILBOX_CLOCK_CONTROL;
typedef struct {
    uint32_t Version, Operation, Id, Reserved;
} PI5_MAILBOX_QUERY;
typedef struct {
    uint32_t Version, Operation, Id, Value;
} PI5_MAILBOX_RESULT;
typedef struct {
    uint32_t Version, Debug, Owned, Fault, Unsafe, Online;
    uint32_t LastStatus, LastTag, Transactions, Failures, Timeouts, ForeignReplies;
    uint32_t RtcReads, RtcWrites, RtcFailures, LastRtcEpoch, LastRtcSetEpoch;
    int32_t LastRtcTimeZone;
    uint32_t OpRegionCalls, RejectedRestarts;
    uint64_t DmaAddress;
} PI5_MAILBOX_STATS;
