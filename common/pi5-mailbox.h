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
    Pi5MailboxClockMaxRate = 6,
    Pi5MailboxClockMeasuredRate = 7
};
/* Kernel-only operations on ARM clock 3 (rate only) and V3D clock 5 (rate/state).
 * Ownership and abandonment are independent for each clock. A controller must claim
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

/* Per-connector queries use firmware display IDs internally. Port 0/1 means
 * the board's HDMI 1/2; callers never select the legacy global framebuffer. */
#define IOCTL_PI5_MAILBOX_DISPLAY_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x853, METHOD_BUFFERED, FILE_READ_DATA)
enum { Pi5MailboxDisplayEdid = 1, Pi5MailboxDisplayTiming = 2 };
typedef struct {
    uint32_t Version, Operation, Port, Block;
} PI5_MAILBOX_DISPLAY_QUERY;
/* Firmware timing layout shared with vc4_firmware_kms, clock in kHz. */
typedef struct {
    uint8_t Display, Padding;
    uint16_t VideoId;
    uint32_t Clock;
    uint16_t HDisplay, HSyncStart, HSyncEnd, HTotal;
    uint16_t HSkew, VDisplay, VSyncStart, VSyncEnd;
    uint16_t VTotal, VScan, VRefresh, Padding2;
    uint32_t Flags;
} PI5_MAILBOX_DISPLAY_TIMING;
typedef struct {
    uint32_t Version, Operation, Port, Block;
    union {
        uint8_t Edid[128];
        PI5_MAILBOX_DISPLAY_TIMING Timing;
    } Data;
} PI5_MAILBOX_DISPLAY_RESULT;
