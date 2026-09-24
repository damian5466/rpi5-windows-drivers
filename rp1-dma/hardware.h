// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <ntddk.h>
#define DMA_REG_SIZE 0x1000u
#define DMA_ID 0x00u
#define DMA_VERSION 0x08u
#define DMA_CONFIG 0x10u
#define DMA_CHANNELS 0x18u
#define DMA_INT_STATUS 0x30u
#define DMA_COMMON_CLEAR 0x38u
#define DMA_COMMON_ENABLE 0x40u
#define DMA_COMMON_SIGNAL 0x48u
#define DMA_COMMON_STATUS 0x50u
#define DMA_RESET 0x58u
#define DMA_CH_BASE 0x100u
#define DMA_CH_CONFIG 0x120u
#define DMA_CH_LIST 0x128u
#define DMA_CH_INT_ENABLE 0x180u
#define DMA_CH_INT_STATUS 0x188u
#define DMA_CH_INT_SIGNAL 0x190u
#define DMA_CH_INT_CLEAR 0x198u
#define DMA_COMPLETE 2u
#define DMA_ERRORS 0x003f7fe0u
#define DMA_BUFFER_LENGTH 65536u
#define DMA_SOURCE_OFFSET 4096u
#define DMA_DEST_OFFSET (DMA_SOURCE_OFFSET + DMA_BUFFER_LENGTH + 4096u)
#define DMA_ALLOCATION (DMA_DEST_OFFSET + DMA_BUFFER_LENGTH + 4096u)
typedef struct {
    ULONGLONG Source, Destination;
    ULONG BlockCount, BlockCountHigh;
    ULONGLONG Next;
    ULONG Control, ControlHigh;
    ULONG SourceStatus, DestinationStatus, Status, StatusHigh, Reserved, ReservedHigh;
} RP1_DMA_DESCRIPTOR;
C_ASSERT(sizeof(RP1_DMA_DESCRIPTOR) == 64);
ULONG DmaRead(PUCHAR Registers, ULONG Offset);
VOID DmaWrite(PUCHAR Registers, ULONG Offset, ULONG Value);
NTSTATUS DmaDescriptor(RP1_DMA_DESCRIPTOR *Descriptor, ULONGLONG Source, ULONGLONG Destination, ULONG Length);
NTSTATUS DmaBegin(PUCHAR Registers, ULONGLONG DescriptorAddress);
BOOLEAN DmaHalt(PUCHAR Registers);
