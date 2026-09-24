// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#define RP1_DMA_NAME L"\\Device\\Pi5Dma"
#define IOCTL_RP1_DMA_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_RP1_DMA_SELFTEST CTL_CODE(FILE_DEVICE_UNKNOWN, 0x821, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_RP1_DMA_COPY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x822, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define RP1_DMA_MAX_COPY 65536u
// COPY is kernel-only: input is this header + Length bytes, output is Length
// copied bytes. All DMA addresses and common-buffer ownership stay in service.
typedef struct { ULONG Version, Length; UCHAR Data[1]; } RP1_DMA_COPY;
typedef struct {
    ULONG Version, Online, ControllerId, ComponentVersion, ClockHz;
    ULONG Interrupts, Transfers, Errors, LastStatus, LastInterrupt, ChannelEnable;
} RP1_DMA_STATUS;
typedef struct {
    ULONG Version, Tests, Bytes, Interrupts, GuardErrors, DataErrors;
} RP1_DMA_TEST_RESULT;
