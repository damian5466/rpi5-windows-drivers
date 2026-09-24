// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <ntddk.h>
#include <wdf.h>
#include <sercx.h>
#include <ntddser.h>
#define UART_RBR 0x00u
#define UART_THR 0x00u
#define UART_DLL 0x00u
#define UART_IER 0x04u
#define UART_DLM 0x04u
#define UART_IIR 0x08u
#define UART_FCR 0x08u
#define UART_LCR 0x0cu
#define UART_MCR 0x10u
#define UART_LSR 0x14u
#define UART_MSR 0x18u
#define UART_SCR 0x1cu
#define UART_RX_IRQ 5u
#define UART_TX_IRQ 2u
#define UART_MODEM_IRQ 8u
#define UART_RX_READY 1u
#define UART_TX_EMPTY 0x20u
#define UART_IDLE 0x40u
#define UART_FIFO 0x41u // FIFO enabled, receive threshold eight bytes.
#define UART_RTS 2u
#define UART_AFE 0x20u
#define UART_RING_SIZE 16384u
#define UART_HIGH_WATER (UART_RING_SIZE - 64u)
#define UART_LOW_WATER (UART_RING_SIZE / 2u)
typedef struct {
    WDFDEVICE Device;
    WDFINTERRUPT Interrupt;
    WDFTIMER QuirkTimer, Telemetry;
    WDFKEY Key;
    SERCX2PIORECEIVE Receive;
    SERCX2PIOTRANSMIT Transmit;
    PUCHAR Registers;
    ULONG ClockHz, Baud, Line, Control, Mask, WaitMask, Events, Errors;
    ULONG Head, Tail, Count;
    ULONG Irqs, RxBytes, TxBytes, Quirks, LastIoctl, LastStatus;
#if DBG
    ULONG TraceCount, Trace[128][3];
#endif
    UCHAR Ring[UART_RING_SIZE];
    SERIAL_HANDFLOW Flow;
    SERIAL_CHARS Chars;
    ULONG Boot[6]; // LCR, MCR, IER, DLL, DLM, SCR.
    BOOLEAN Online, Open, RxArmed, RxPending, TxArmed, TxPending;
    BOOLEAN RxPaused, Quirk, QuirkPending;
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
VOID UartInterruptMask(UART_CONTEXT *c);
VOID UartTrace(UART_CONTEXT *c, ULONG Kind, ULONG Value, ULONG Detail);
