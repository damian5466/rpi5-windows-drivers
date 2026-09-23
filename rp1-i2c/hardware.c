// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"
ULONG I2cRead(I2C_ENGINE *e, ULONG Offset)
{ return READ_REGISTER_ULONG((PULONG)(e->Registers + Offset)); }
VOID I2cWrite(I2C_ENGINE *e, ULONG Offset, ULONG Value)
{ WRITE_REGISTER_ULONG((PULONG)(e->Registers + Offset), Value); }
static ULONG Cycles(ULONG hz, ULONG ns)
{ return (ULONG)(((ULONGLONG)hz * ns + 999999999) / 1000000000); }
NTSTATUS I2cTiming(ULONG Hz, ULONG Speed, ULONG *High, ULONG *Low, ULONG *Hold)
{
    ULONG high, low, period;
    if ((Speed != 100000 && Speed != 400000) || Hz < 1000000 || Hz > 250000000)
        return STATUS_INVALID_PARAMETER;
    // DesignWare counts include internal offsets of 3 and 1 cycles.
    // Budget 300 ns for signal fall time and never exceed the requested rate.
    high = Cycles(Hz, Speed == 100000 ? 4300 : 900);
    low = Cycles(Hz, Speed == 100000 ? 5000 : 1600);
    high = high > 3 ? high - 3 : 6;
    low = low > 1 ? low - 1 : 8;
    if (high < 6) high = 6;
    if (low < 8) low = 8;
    period = (Hz + Speed - 1) / Speed;
    if (high + low < period) low = period - high;
    if (high > 65535 || low > 65535) return STATUS_INVALID_PARAMETER;
    *High = high; *Low = low;
    *Hold = Cycles(Hz, 300) | (1u << 16);
    return STATUS_SUCCESS;
}
NTSTATUS I2cPump(I2C_ENGINE *e, ULONG *Mask, BOOLEAN *Done)
{
    ULONG raw = I2cRead(e, I2C_RAW), available, count;
    *Mask = 0; *Done = FALSE;
    if (raw & 0x40) {
        e->AbortSource = I2cRead(e, I2C_ABORT_SOURCE);
        (void)I2cRead(e, I2C_CLEAR_ABORT);
        return (e->AbortSource & 7) ? STATUS_NO_SUCH_DEVICE : STATUS_IO_DEVICE_ERROR;
    }
    if (raw & 0x0a) return STATUS_DATA_OVERRUN;
    if (raw & I2C_IRQ_STOP) { e->Stopped = TRUE; (void)I2cRead(e, I2C_CLEAR_STOP); }
    count = I2cRead(e, I2C_RX_LEVEL);
    if (count > e->RxDepth || count > e->Outstanding) return STATUS_DEVICE_DATA_ERROR;
    while (count--) {
        I2C_TRANSFER *t;
        while (e->RxIndex < e->TransferCount &&
            (!e->Transfers[e->RxIndex].Read || e->Transfers[e->RxIndex].Received == e->Transfers[e->RxIndex].Length)) ++e->RxIndex;
        if (e->RxIndex == e->TransferCount) return STATUS_DEVICE_DATA_ERROR;
        t = &e->Transfers[e->RxIndex];
        e->Buffer[t->Offset + t->Received++] = (UCHAR)I2cRead(e, I2C_DATA);
        --e->Outstanding;
    }
    if (e->Stopped) {
        if (e->TxIndex != e->TransferCount || e->Outstanding) return STATUS_DEVICE_DATA_ERROR;
        *Done = TRUE;
        return STATUS_SUCCESS;
    }
    count = I2cRead(e, I2C_TX_LEVEL);
    if (count > e->TxDepth) return STATUS_DEVICE_DATA_ERROR;
    available = e->TxDepth - count;
    while (available && e->TxIndex < e->TransferCount) {
        I2C_TRANSFER *t = &e->Transfers[e->TxIndex];
        ULONG command;
        if (t->Read && e->Outstanding == e->RxDepth) break;
        command = t->Read ? I2C_COMMAND_READ : e->Buffer[t->Offset + t->Queued];
        if (!t->Queued && e->TxIndex) command |= I2C_COMMAND_RESTART;
        if (e->TxIndex + 1 == e->TransferCount && t->Queued + 1 == t->Length) command |= I2C_COMMAND_STOP;
        I2cWrite(e, I2C_DATA, command);
        if (t->Read) ++e->Outstanding;
        ++t->Queued; --available;
        if (t->Queued == t->Length) ++e->TxIndex;
    }
    *Mask = I2C_IRQ_ERRORS | I2C_IRQ_STOP;
    if (e->Outstanding) *Mask |= I2C_IRQ_RX;
    if (e->TxIndex < e->TransferCount &&
        (!e->Transfers[e->TxIndex].Read || e->Outstanding < e->RxDepth)) *Mask |= I2C_IRQ_TX;
    return STATUS_SUCCESS;
}
