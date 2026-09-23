// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"
ULONG SpiRead(SPI_ENGINE *e, ULONG Offset)
{ return READ_REGISTER_ULONG((PULONG)(e->Registers + Offset)); }
VOID SpiWrite(SPI_ENGINE *e, ULONG Offset, ULONG Value)
{ WRITE_REGISTER_ULONG((PULONG)(e->Registers + Offset), Value); }
NTSTATUS SpiFrameControl(SPI_ENGINE *e, ULONG *Control)
{
    ULONG saved = SpiRead(e, SPI_CONTROL), base = saved & ~0x1f000fu;
    NTSTATUS status = STATUS_SUCCESS;
    // DFS placement is a synthesis option, not implied by the IP version.
    // Probe only with SSI disabled and restore the entire original register.
    if (SpiRead(e, SPI_ENABLE)) return STATUS_DEVICE_BUSY;
    SpiWrite(e, SPI_CONTROL, base | 7);
    if ((SpiRead(e, SPI_CONTROL) & 15) == 7) *Control = 7;
    else {
        SpiWrite(e, SPI_CONTROL, base | (7u << 16));
        if ((SpiRead(e, SPI_CONTROL) & 0x1f0000) == (7u << 16)) *Control = 7u << 16;
        else status = STATUS_NOT_SUPPORTED;
    }
    SpiWrite(e, SPI_CONTROL, saved);
    return status;
}
NTSTATUS SpiDivider(ULONG Hz, ULONG Speed, ULONG *Divider)
{
    ULONG divider;
    if (Speed < 100000 || Speed > 4000000 || Hz < Speed || Hz > 250000000)
        return STATUS_INVALID_PARAMETER;
    divider = (Hz + Speed - 1) / Speed;
    divider = (divider + 1) & ~1u;
    if (divider < 2 || divider > 65534) return STATUS_INVALID_PARAMETER;
    *Divider = divider;
    return STATUS_SUCCESS;
}
NTSTATUS SpiPump(SPI_ENGINE *e, ULONG *Mask, BOOLEAN *Done)
{
    ULONG count, available;
    *Mask = 0; *Done = FALSE;
    if (SpiRead(e, SPI_RAW) & SPI_IRQ_ERRORS) return STATUS_IO_DEVICE_ERROR;
    count = SpiRead(e, SPI_RX_LEVEL);
    if (count > e->Depth || count > e->Queued - e->Received) return STATUS_DEVICE_DATA_ERROR;
    while (count--) e->Rx[e->Received++] = (UCHAR)SpiRead(e, SPI_DATA);
    if (e->Received == e->Frames) {
        *Done = !(SpiRead(e, SPI_STATUS) & 1);
        return STATUS_SUCCESS;
    }
    count = SpiRead(e, SPI_TX_LEVEL);
    if (count > e->Depth) return STATUS_DEVICE_DATA_ERROR;
    available = e->Depth - count;
    // Account for bytes in both FIFOs AND the shift register. The controller
    // must never clock more input than the receive FIFO has room to hold.
    count = e->Depth - (e->Queued - e->Received);
    if (available > count) available = count;
    while (available-- && e->Queued < e->Frames) SpiWrite(e, SPI_DATA, e->Tx[e->Queued++]);
    *Mask = SPI_IRQ_ERRORS | SPI_IRQ_RX;
    if (e->Queued < e->Frames && e->Queued - e->Received < e->Depth) *Mask |= SPI_IRQ_TX;
    return STATUS_SUCCESS;
}
