// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"
VOID UartTrace(UART_CONTEXT *c, ULONG Kind, ULONG Value, ULONG Detail)
{
#if DBG
    ULONG *entry = c->Trace[c->TraceCount++ % 128];
    entry[0] = Kind; entry[1] = Value; entry[2] = Detail;
#else
    UNREFERENCED_PARAMETER(c); UNREFERENCED_PARAMETER(Kind);
    UNREFERENCED_PARAMETER(Value); UNREFERENCED_PARAMETER(Detail);
#endif
}
ULONG UartRead(UART_CONTEXT *c, ULONG Offset)
{ return READ_REGISTER_ULONG((PULONG)(c->Registers + Offset)); }
VOID UartWrite(UART_CONTEXT *c, ULONG Offset, ULONG Value)
{ UartTrace(c, 1, Offset, Value); WRITE_REGISTER_ULONG((PULONG)(c->Registers + Offset), Value); }
NTSTATUS UartDivisor(ULONG ClockHz, ULONG Baud, ULONG *Divisor)
{
    ULONGLONG value, actual, error;
    if (Baud < 300 || Baud > 3000000 || ClockHz < 16ull * Baud) return STATUS_INVALID_PARAMETER;
    value = (ClockHz + 8ull * Baud) / (16ull * Baud);
    if (!value || value > 65535) return STATUS_INVALID_PARAMETER;
    actual = ClockHz / (16 * value); error = actual > Baud ? actual - Baud : Baud - actual;
    if (error * 100 > Baud * 2ull) return STATUS_INVALID_PARAMETER;
    *Divisor = (ULONG)value; return STATUS_SUCCESS;
}
NTSTATUS UartLine(const SERIAL_LINE_CONTROL *Line, ULONG *Value)
{
    ULONG value;
    if (Line->WordLength < 5 || Line->WordLength > 8) return STATUS_INVALID_PARAMETER;
    value = Line->WordLength - 5u;
    if (Line->StopBits == STOP_BITS_2) value |= 4;
    else if (Line->StopBits != STOP_BIT_1) return STATUS_INVALID_PARAMETER;
    switch (Line->Parity) {
    case NO_PARITY: break;
    case ODD_PARITY: value |= 8; break;
    case EVEN_PARITY: value |= 0x18; break;
    case MARK_PARITY: value |= 0x28; break;
    case SPACE_PARITY: value |= 0x38; break;
    default: return STATUS_INVALID_PARAMETER;
    }
    *Value = value; return STATUS_SUCCESS;
}
VOID UartInterruptMask(UART_CONTEXT *c)
{
    ULONG control = c->Open ? c->Control : 0;
    if (c->RxPaused || c->Quirk) control &= ~UART_RTS;
    c->Mask = c->Open && c->Online ? UART_MODEM_IRQ |
        ((c->RxPaused || c->Quirk) ? 0 : UART_RX_IRQ) | (c->TxArmed ? UART_TX_IRQ : 0) : 0;
    UartWrite(c, UART_MCR, control);
    UartWrite(c, UART_IER, c->Mask);
    (void)UartRead(c, UART_IER);
}
VOID UartProgram(UART_CONTEXT *c)
{
    ULONG divisor = 0;
    (void)UartDivisor(c->ClockHz, c->Baud, &divisor);
    UartWrite(c, UART_IER, 0);
    UartWrite(c, UART_LCR, c->Line | 0x80);
    UartWrite(c, UART_DLL, divisor & 255);
    UartWrite(c, UART_DLM, divisor >> 8);
    UartWrite(c, UART_LCR, c->Line);
    UartInterruptMask(c);
}
VOID UartCapture(UART_CONTEXT *c)
{
    ULONG i;
    for (i = 0; i < 64 && c->Count < UART_HIGH_WATER; ++i) {
        ULONG state = UartRead(c, UART_LSR), data;
        if (state & 2) c->Errors |= SERIAL_ERROR_OVERRUN;
        if (state & 4) c->Errors |= SERIAL_ERROR_PARITY;
        if (state & 8) c->Errors |= SERIAL_ERROR_FRAMING;
        if (state & 16) { c->Errors |= SERIAL_ERROR_BREAK; c->Events |= SERIAL_EV_BREAK; }
        if (!(state & UART_RX_READY)) break;
        data = UartRead(c, UART_RBR);
        UartTrace(c, 2, data, state);
        c->Ring[c->Head] = (UCHAR)data;
        c->Head = (c->Head + 1) % UART_RING_SIZE; ++c->Count; ++c->RxBytes;
        c->Events |= SERIAL_EV_RXCHAR;
        if ((UCHAR)data == (UCHAR)c->Chars.EventChar) c->Events |= SERIAL_EV_RXFLAG;
    }
    // Automatic RTS protects the hardware FIFO; this also protects the ring.
    if (c->Count >= UART_HIGH_WATER && !c->RxPaused) {
        c->RxPaused = TRUE; UartInterruptMask(c);
    }
    if (c->Errors) c->Events |= SERIAL_EV_ERR;
}
