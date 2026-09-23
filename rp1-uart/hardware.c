// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"
ULONG UartRead(UART_CONTEXT *c, ULONG Offset)
{ return READ_REGISTER_ULONG((PULONG)(c->Registers + Offset)); }
VOID UartWrite(UART_CONTEXT *c, ULONG Offset, ULONG Value)
{ WRITE_REGISTER_ULONG((PULONG)(c->Registers + Offset), Value); }
NTSTATUS UartDivisor(ULONG ClockHz, ULONG Baud, ULONG *Divisor)
{
    ULONGLONG value, actual, error;
    if (Baud < 300 || Baud > 1000000 || ClockHz < 16ull * Baud) return STATUS_INVALID_PARAMETER;
    value = (4ull * ClockHz + Baud / 2) / Baud;
    if (value < 64 || value > 0x3fffff) return STATUS_INVALID_PARAMETER;
    actual = 4ull * ClockHz / value;
    error = actual > Baud ? actual - Baud : Baud - actual;
    if (error * 100 > Baud * 2ull) return STATUS_INVALID_PARAMETER;
    *Divisor = (ULONG)value;
    return STATUS_SUCCESS;
}
NTSTATUS UartLine(const SERIAL_LINE_CONTROL *Line, ULONG *Value)
{
    ULONG value;
    if (Line->WordLength < 5 || Line->WordLength > 8) return STATUS_INVALID_PARAMETER;
    value = 0x10u | ((Line->WordLength - 5u) << 5);
    if (Line->StopBits == STOP_BITS_2) value |= 8;
    else if (Line->StopBits != STOP_BIT_1) return STATUS_INVALID_PARAMETER;
    switch (Line->Parity) {
    case NO_PARITY: break;
    case ODD_PARITY: value |= 2; break;
    case EVEN_PARITY: value |= 6; break;
    case MARK_PARITY: value |= 0x82; break;
    case SPACE_PARITY: value |= 0x86; break;
    default: return STATUS_INVALID_PARAMETER;
    }
    *Value = value;
    return STATUS_SUCCESS;
}
VOID UartProgram(UART_CONTEXT *c)
{
    ULONG divisor = 0;
    (void)UartDivisor(c->ClockHz, c->Baud, &divisor);
    UartWrite(c, UART_CR, 0);
    UartWrite(c, UART_IBRD, divisor >> 6);
    UartWrite(c, UART_FBRD, divisor & 63);
    UartWrite(c, UART_LCRH, c->Line); // Commits both divisor registers.
    UartWrite(c, UART_CR, c->Open ? c->Control : 0);
    (void)UartRead(c, UART_CR);
}
// Called with the interrupt lock held. Bound each FIFO drain so a noisy
// external transmitter cannot monopolize a shared system interrupt.
VOID UartCapture(UART_CONTEXT *c)
{
    ULONG i;
    for (i = 0; i < 64 && !(UartRead(c, UART_FR) & UART_RX_EMPTY); ++i) {
        ULONG data = UartRead(c, UART_DR);
        if (data & 0x100) c->Errors |= SERIAL_ERROR_FRAMING;
        if (data & 0x200) c->Errors |= SERIAL_ERROR_PARITY;
        if (data & 0x400) { c->Errors |= SERIAL_ERROR_BREAK; c->Events |= SERIAL_EV_BREAK; }
        if (data & 0x800) c->Errors |= SERIAL_ERROR_OVERRUN;
        if (c->Count == UART_RING_SIZE) c->Errors |= SERIAL_ERROR_QUEUEOVERRUN;
        else {
            c->Ring[c->Head] = (UCHAR)data;
            c->Head = (c->Head + 1) % UART_RING_SIZE; ++c->Count;
            c->Events |= SERIAL_EV_RXCHAR;
            if ((UCHAR)data == (UCHAR)c->Chars.EventChar) c->Events |= SERIAL_EV_RXFLAG;
        }
    }
    if (c->Errors) c->Events |= SERIAL_EV_ERR;
}
