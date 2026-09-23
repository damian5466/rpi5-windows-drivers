// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <ntddk.h>
#include <wdf.h>
#include <sercx.h>
#include <ntddser.h>
#define UART_DR 0x00
#define UART_RSR 0x04
#define UART_FR 0x18
#define UART_IBRD 0x24
#define UART_FBRD 0x28
#define UART_LCRH 0x2c
#define UART_CR 0x30
#define UART_IFLS 0x34
#define UART_IMSC 0x38
#define UART_MIS 0x40
#define UART_ICR 0x44
#define UART_DMACR 0x48
#define UART_RX_IRQ 0x7d0u
#define UART_TX_IRQ 0x20u
#define UART_RX_EMPTY 0x10u
#define UART_TX_FULL 0x20u
#define UART_BUSY 0x08u
#define UART_RING_SIZE 16384u
typedef struct {
    WDFDEVICE Device;
    WDFINTERRUPT Interrupt;
    WDFTIMER DrainTimer;
    WDFIOTARGET Route, Clock;
    SERCX2PIORECEIVE Receive;
    SERCX2PIOTRANSMIT Transmit;
    PUCHAR Registers;
    ULONG Uid, ClockHz, Baud, Line, Control, Mask, WaitMask, Events, Errors;
    ULONG Head, Tail, Count;
    UCHAR Ring[UART_RING_SIZE];
    SERIAL_HANDFLOW Flow;
    SERIAL_CHARS Chars;
    ULONG Boot[7];
    BOOLEAN Online, Open, RxArmed, RxPending, TxArmed, TxPending, Draining, Purging;
} UART_CONTEXT;
typedef struct { UART_CONTEXT *Controller; } UART_PIO_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(UART_CONTEXT, UartContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(UART_PIO_CONTEXT, UartPioContext)
ULONG UartRead(UART_CONTEXT *c, ULONG Offset);
VOID UartWrite(UART_CONTEXT *c, ULONG Offset, ULONG Value);
NTSTATUS UartDivisor(ULONG ClockHz, ULONG Baud, ULONG *Divisor);
NTSTATUS UartLine(const SERIAL_LINE_CONTROL *Line, ULONG *Value);
VOID UartProgram(UART_CONTEXT *c);
VOID UartCapture(UART_CONTEXT *c);
