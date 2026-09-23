// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <ntddk.h>
#define SPI_CONTROL 0x00
#define SPI_COUNT 0x04
#define SPI_ENABLE 0x08
#define SPI_SELECT 0x10
#define SPI_DIVIDER 0x14
#define SPI_TX_THRESHOLD 0x18
#define SPI_RX_THRESHOLD 0x1c
#define SPI_TX_LEVEL 0x20
#define SPI_RX_LEVEL 0x24
#define SPI_STATUS 0x28
#define SPI_MASK 0x2c
#define SPI_INTR 0x30
#define SPI_RAW 0x34
#define SPI_CLEAR 0x48
#define SPI_DMA 0x4c
#define SPI_VERSION 0x5c
#define SPI_DATA 0x60
#define SPI_RX_DELAY 0xf0
#define SPI_IRQ_ERRORS 0x2e
#define SPI_IRQ_RX 0x10
#define SPI_IRQ_TX 1
#define SPI_MAX_TRANSFERS 16
#define SPI_MAX_BYTES 65536
typedef struct {
    PUCHAR Registers, Tx, Rx;
    ULONG Depth, Frames, Queued, Received;
} SPI_ENGINE;
ULONG SpiRead(SPI_ENGINE *e, ULONG Offset);
VOID SpiWrite(SPI_ENGINE *e, ULONG Offset, ULONG Value);
NTSTATUS SpiDivider(ULONG Hz, ULONG Speed, ULONG *Divider);
NTSTATUS SpiFrameControl(SPI_ENGINE *e, ULONG *Control);
NTSTATUS SpiPump(SPI_ENGINE *e, ULONG *Mask, BOOLEAN *Done);
