// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <ntddk.h>
#define I2C_CON 0x00
#define I2C_TAR 0x04
#define I2C_DATA 0x10
#define I2C_SS_HIGH 0x14
#define I2C_SS_LOW 0x18
#define I2C_FS_HIGH 0x1c
#define I2C_FS_LOW 0x20
#define I2C_INTR 0x2c
#define I2C_MASK 0x30
#define I2C_RAW 0x34
#define I2C_RX_TL 0x38
#define I2C_TX_TL 0x3c
#define I2C_CLEAR 0x40
#define I2C_CLEAR_ABORT 0x54
#define I2C_CLEAR_STOP 0x60
#define I2C_ENABLE 0x6c
#define I2C_STATUS 0x70
#define I2C_TX_LEVEL 0x74
#define I2C_RX_LEVEL 0x78
#define I2C_SDA_HOLD 0x7c
#define I2C_ABORT_SOURCE 0x80
#define I2C_DMA 0x88
#define I2C_ENABLE_STATUS 0x9c
#define I2C_PARAMETERS 0xf4
#define I2C_VERSION 0xf8
#define I2C_TYPE 0xfc
#define I2C_IRQ_ERRORS 0x4au
#define I2C_IRQ_STOP 0x200u
#define I2C_IRQ_RX 4u
#define I2C_IRQ_TX 0x10u
#define I2C_COMMAND_READ 0x100u
#define I2C_COMMAND_STOP 0x200u
#define I2C_COMMAND_RESTART 0x400u
#define I2C_MAX_TRANSFERS 16
#define I2C_MAX_BYTES 65536
typedef struct {
    ULONG Offset, Length, Queued, Received;
    BOOLEAN Read;
} I2C_TRANSFER;
typedef struct {
    PUCHAR Registers, Buffer;
    ULONG TxDepth, RxDepth, TransferCount, TxIndex, RxIndex, Outstanding;
    ULONG AbortSource;
    BOOLEAN Stopped;
    I2C_TRANSFER Transfers[I2C_MAX_TRANSFERS];
} I2C_ENGINE;
ULONG I2cRead(I2C_ENGINE *e, ULONG Offset);
VOID I2cWrite(I2C_ENGINE *e, ULONG Offset, ULONG Value);
NTSTATUS I2cTiming(ULONG Hz, ULONG Speed, ULONG *High, ULONG *Low, ULONG *Hold);
NTSTATUS I2cPump(I2C_ENGINE *e, ULONG *Mask, BOOLEAN *Done);
