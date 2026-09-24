// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"
ULONG DmaRead(PUCHAR Registers, ULONG Offset)
{ return READ_REGISTER_ULONG((PULONG)(Registers + Offset)); }
VOID DmaWrite(PUCHAR Registers, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Registers + Offset), Value);
    (void)DmaRead(Registers, DMA_CONFIG); // drain posted PCIe writes
}
static VOID WriteAddress(PUCHAR Registers, ULONG Offset, ULONGLONG Address)
{
    DmaWrite(Registers, Offset, (ULONG)Address);
    DmaWrite(Registers, Offset + 4, (ULONG)(Address >> 32));
}
NTSTATUS DmaDescriptor(RP1_DMA_DESCRIPTOR *d, ULONGLONG Source, ULONGLONG Destination, ULONG Length)
{
    ULONG width = 0;
    ULONGLONG alignment = Source | Destination | Length;
    if (!Length || Length > DMA_BUFFER_LENGTH || Source > 0xffffffffULL - Length + 1 ||
        Destination > 0xffffffffULL - Length + 1) return STATUS_INVALID_PARAMETER;
    while (width < 4 && !(alignment & (1ull << width))) ++width;
    RtlZeroMemory(d, sizeof(*d));
    d->Source = Source; d->Destination = Destination;
    d->BlockCount = (Length >> width) - 1;
    // Incrementing memory endpoints, AXI master0, four transfers/request.
    d->Control = (1u << 18) | (1u << 14) | (width << 11) | (width << 8);
    // One final valid LLI; bound AXI read/write bursts to one beat. The slow
    // initial copy service does not consume streaming peripherals' bandwidth.
    d->ControlHigh = (1u << 31) | (1u << 30) | (1u << 15) | (1u << 6);
    return STATUS_SUCCESS;
}
NTSTATUS DmaBegin(PUCHAR Registers, ULONGLONG DescriptorAddress)
{
    if ((DescriptorAddress & 63) || DescriptorAddress > 0xffffffc0ULL)
        return STATUS_INVALID_PARAMETER;
    if (DmaRead(Registers, DMA_CHANNELS) & 1) return STATUS_DEVICE_BUSY;
    DmaWrite(Registers, DMA_CH_INT_SIGNAL, 0);
    DmaWrite(Registers, DMA_CH_INT_CLEAR, 0xffffffffu);
    // Linked-list source and destination; memory-to-memory DMA flow control.
    DmaWrite(Registers, DMA_CH_CONFIG, 15);
    DmaWrite(Registers, DMA_CH_CONFIG + 4, 0);
    WriteAddress(Registers, DMA_CH_LIST, DescriptorAddress);
    DmaWrite(Registers, DMA_CH_INT_ENABLE, DMA_COMPLETE | DMA_ERRORS);
    DmaWrite(Registers, DMA_CH_INT_SIGNAL, DMA_COMPLETE | DMA_ERRORS);
    KeMemoryBarrier();
    DmaWrite(Registers, DMA_CHANNELS, 0x101); // channel0 enable + write-enable
    return STATUS_SUCCESS;
}
BOOLEAN DmaHalt(PUCHAR Registers)
{
    ULONG i;
    DmaWrite(Registers, DMA_CH_INT_SIGNAL, 0);
    DmaWrite(Registers, DMA_CHANNELS, 0x100); // clear only channel0's enable
    for (i = 0; i < 1000; ++i) {
        if (!(DmaRead(Registers, DMA_CHANNELS) & 1)) return TRUE;
        KeStallExecutionProcessor(1);
    }
    // This driver exclusively owns the controller and refused any boot-active
    // channel. A bounded reset is the final recovery for an outstanding AXI IO.
    DmaWrite(Registers, DMA_RESET, 1);
    for (i = 0; i < 1000; ++i) {
        if (!(DmaRead(Registers, DMA_RESET) & 1) && !(DmaRead(Registers, DMA_CHANNELS) & 0xff)) return TRUE;
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}
