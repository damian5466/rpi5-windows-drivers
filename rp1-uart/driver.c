// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"
#define RP1_SERVICE_CLIENT
#define RP1_CLOCK_CLIENT
#include "rp1-service.h"
#include "rp1-clock.h"
#include "rp1-acpi.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD UartAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE UartPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE UartRelease;
EVT_WDF_DEVICE_D0_ENTRY UartStart;
EVT_WDF_DEVICE_D0_EXIT UartStop;
EVT_WDF_INTERRUPT_ISR UartIsr;
EVT_WDF_INTERRUPT_DPC UartDpc;
EVT_WDF_INTERRUPT_ENABLE UartInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE UartInterruptDisable;
EVT_WDF_TIMER UartDrainTimer;
EVT_SERCX2_FILEOPEN UartOpen;
EVT_SERCX2_FILECLOSE UartClose;
EVT_SERCX2_CONTROL UartControl;
EVT_SERCX2_APPLY_CONFIG UartApply;
EVT_SERCX2_PURGE_FIFOS UartPurge;
EVT_SERCX2_SET_WAIT_MASK UartWait;
EVT_SERCX2_PIO_RECEIVE_READ_BUFFER UartReceive;
EVT_SERCX2_PIO_RECEIVE_ENABLE_READY_NOTIFICATION UartRxEnable;
EVT_SERCX2_PIO_RECEIVE_CANCEL_READY_NOTIFICATION UartRxCancel;
EVT_SERCX2_PIO_TRANSMIT_WRITE_BUFFER UartTransmit;
EVT_SERCX2_PIO_TRANSMIT_ENABLE_READY_NOTIFICATION UartTxEnable;
EVT_SERCX2_PIO_TRANSMIT_CANCEL_READY_NOTIFICATION UartTxCancel;
EVT_SERCX2_PIO_TRANSMIT_DRAIN_FIFO UartDrain;
EVT_SERCX2_PIO_TRANSMIT_CANCEL_DRAIN_FIFO UartDrainCancel;
EVT_SERCX2_PIO_TRANSMIT_PURGE_FIFO UartTxPurge;
static const ULONG SavedOffsets[] = {UART_IBRD, UART_FBRD, UART_LCRH, UART_CR, UART_IFLS, UART_IMSC, UART_DMACR};

static VOID InterruptMask(UART_CONTEXT *c)
{
    c->Mask = c->Open ? UART_RX_IRQ | (c->TxArmed ? UART_TX_IRQ : 0) : 0;
    UartWrite(c, UART_IMSC, c->Mask);
    (void)UartRead(c, UART_IMSC);
}
_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT Object, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, UartAdd);
    return WdfDriverCreate(Object, Path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS UartAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_INTERRUPT_CONFIG interrupt;
    WDF_TIMER_CONFIG timer;
    SERCX2_CONFIG serial;
    SERCX2_PIO_RECEIVE_CONFIG rx;
    SERCX2_PIO_TRANSMIT_CONFIG tx;
    WDFDEVICE device;
    UART_CONTEXT *c;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = UartPrepare; pnp.EvtDeviceReleaseHardware = UartRelease;
    pnp.EvtDeviceD0Entry = UartStart; pnp.EvtDeviceD0Exit = UartStop;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    status = SerCx2InitializeDeviceInit(Init);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, UART_CONTEXT);
    status = WdfDeviceCreate(&Init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    c = UartContext(device); c->Device = device; c->Baud = 115200; c->Line = 0x70; c->Control = 0x301;
    WDF_INTERRUPT_CONFIG_INIT(&interrupt, UartIsr, UartDpc);
    interrupt.EvtInterruptEnable = UartInterruptEnable;
    interrupt.EvtInterruptDisable = UartInterruptDisable;
    status = WdfInterruptCreate(device, &interrupt, WDF_NO_OBJECT_ATTRIBUTES, &c->Interrupt);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG_INIT(&timer, UartDrainTimer);
    timer.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = device;
    status = WdfTimerCreate(&timer, &a, &c->DrainTimer);
    if (!NT_SUCCESS(status)) return status;
    SERCX2_CONFIG_INIT(&serial, UartApply, UartControl, UartPurge);
    serial.EvtSerCx2FileOpen = UartOpen; serial.EvtSerCx2FileClose = UartClose;
    serial.EvtSerCx2SetWaitMask = UartWait;
    status = SerCx2InitializeDevice(device, &serial);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, UART_PIO_CONTEXT);
    SERCX2_PIO_RECEIVE_CONFIG_INIT(&rx, UartReceive, UartRxEnable, UartRxCancel);
    status = SerCx2PioReceiveCreate(device, &rx, &a, &c->Receive);
    if (!NT_SUCCESS(status)) return status;
    UartPioContext(c->Receive)->Controller = c;
    SERCX2_PIO_TRANSMIT_CONFIG_INIT(&tx, UartTransmit, UartTxEnable, UartTxCancel);
    tx.EvtSerCx2PioTransmitDrainFifo = UartDrain;
    tx.EvtSerCx2PioTransmitCancelDrainFifo = UartDrainCancel;
    tx.EvtSerCx2PioTransmitPurgeFifo = UartTxPurge;
    status = SerCx2PioTransmitCreate(device, &tx, &a, &c->Transmit);
    if (NT_SUCCESS(status)) UartPioContext(c->Transmit)->Controller = c;
    return status;
}
_Use_decl_annotations_
NTSTATUS UartPrepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UART_CONTEXT *c = UartContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    ULONG i, interrupts = 0, pins = 0;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Raw);
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeMemory) {
            if (memory || r->u.Memory.Length != 0x100) return STATUS_DEVICE_CONFIGURATION_ERROR;
            memory = r;
        } else if (r->Type == CmResourceTypeInterrupt) {
            if ((r->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED)) ||
                r->ShareDisposition != CmResourceShareShared) return STATUS_DEVICE_CONFIGURATION_ERROR;
            ++interrupts;
        } else if (r->Type == CmResourceTypeConnection &&
            r->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_FUNCTION_CONFIG &&
            r->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_FUNCTION_CONFIG) ++pins;
    }
    if (!memory || interrupts != 1 || pins != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
    status = Rp1GetUid(Device, &c->Uid);
    if (!NT_SUCCESS(status)) return status;
    if (c->Uid != 0 && c->Uid != 2 && c->Uid != 3 && c->Uid != 4) return STATUS_NOT_SUPPORTED;
    c->Registers = MmMapIoSpaceEx(memory->u.Memory.Start, 0x100, PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->Registers) return STATUS_INSUFFICIENT_RESOURCES;
    for (i = 0; i < RTL_NUMBER_OF(SavedOffsets); ++i) c->Boot[i] = UartRead(c, SavedOffsets[i]);
    // An already active UART is not an implicit firmware handoff.
    if ((c->Boot[3] & 1) || c->Boot[5] || c->Boot[6]) {
        MmUnmapIoSpace(c->Registers, 0x100); c->Registers = NULL;
        return STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS UartRelease(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UART_CONTEXT *c = UartContext(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (c->Registers) { MmUnmapIoSpace(c->Registers, 0x100); c->Registers = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS UartStart(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    UART_CONTEXT *c = UartContext(Device);
    NTSTATUS status;
    ULONG divisor;
    UNREFERENCED_PARAMETER(Previous);
    status = Rp1OpenClock(Device, TRUE, &c->Clock, &c->ClockHz);
    if (!NT_SUCCESS(status)) return status;
    status = UartDivisor(c->ClockHz, c->Baud, &divisor);
    if (NT_SUCCESS(status)) {
        UartWrite(c, UART_IMSC, 0); UartWrite(c, UART_CR, 0);
        UartWrite(c, UART_DMACR, 0); UartWrite(c, UART_ICR, 0x7ff);
        UartWrite(c, UART_IFLS, 0); // RX/TX thresholds: 1/8 of the 32-byte FIFO.
        UartProgram(c);
        status = Rp1OpenInterruptRoute(Device, c->Uid ? c->Uid + 41 : 25, &c->Route);
    }
    if (!NT_SUCCESS(status)) { WdfObjectDelete(c->Clock); c->Clock = NULL; return status; }
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "Pi5Uart: UART%lu online, baud clock %lu Hz\n", c->Uid, c->ClockHz);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS UartStop(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    UART_CONTEXT *c = UartContext(Device);
    ULONG i;
    UNREFERENCED_PARAMETER(Target);
    WdfInterruptAcquireLock(c->Interrupt);
    c->Online = FALSE; c->Draining = c->Purging = FALSE;
    UartWrite(c, UART_IMSC, 0); UartWrite(c, UART_CR, 0);
    (void)UartRead(c, UART_CR);
    WdfInterruptReleaseLock(c->Interrupt);
    (void)WdfTimerStop(c->DrainTimer, TRUE);
    if (c->Route) { WdfObjectDelete(c->Route); c->Route = NULL; }
    for (i = 0; i < RTL_NUMBER_OF(SavedOffsets); ++i) UartWrite(c, SavedOffsets[i], c->Boot[i]);
    (void)UartRead(c, UART_CR);
    if (c->Clock) { WdfObjectDelete(c->Clock); c->Clock = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS UartOpen(WDFDEVICE Device)
{
    UART_CONTEXT *c = UartContext(Device);
    NTSTATUS status = STATUS_SUCCESS;
    WdfInterruptAcquireLock(c->Interrupt);
    if (!c->Online || c->Open) status = STATUS_DEVICE_NOT_READY;
    else {
        c->Head = c->Tail = c->Count = c->Errors = c->Events = c->WaitMask = 0;
        c->RxArmed = c->RxPending = c->TxArmed = c->TxPending = c->Draining = c->Purging = FALSE;
        c->Baud = 115200; c->Line = 0x70; c->Control = 0x301;
        RtlZeroMemory(&c->Flow, sizeof(c->Flow)); RtlZeroMemory(&c->Chars, sizeof(c->Chars));
        c->Open = TRUE;
        UartWrite(c, UART_LCRH, 0); UartWrite(c, UART_RSR, 0);
        UartProgram(c); UartWrite(c, UART_ICR, 0x7ff); InterruptMask(c);
    }
    WdfInterruptReleaseLock(c->Interrupt);
    return status;
}
_Use_decl_annotations_
VOID UartClose(WDFDEVICE Device)
{
    UART_CONTEXT *c = UartContext(Device);
    WdfInterruptAcquireLock(c->Interrupt);
    c->Open = FALSE; c->Draining = c->Purging = FALSE;
    InterruptMask(c); UartWrite(c, UART_CR, 0); (void)UartRead(c, UART_CR);
    c->Head = c->Tail = c->Count = c->WaitMask = c->Events = 0;
    WdfInterruptReleaseLock(c->Interrupt);
    (void)WdfTimerStop(c->DrainTimer, FALSE);
}
_Use_decl_annotations_
NTSTATUS UartApply(WDFDEVICE Device, PVOID Parameters)
{
    // These ports are exposed through SerCx-FriendlyName, without a fixed
    // ACPI peripheral connection. Applications configure them with serial IOCTLs.
    UNREFERENCED_PARAMETER(Device); UNREFERENCED_PARAMETER(Parameters);
    return STATUS_NOT_SUPPORTED;
}
_Use_decl_annotations_
NTSTATUS UartInterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    UART_CONTEXT *c = UartContext(Device);
    UNREFERENCED_PARAMETER(Interrupt);
    c->Online = TRUE; InterruptMask(c);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS UartInterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    UART_CONTEXT *c = UartContext(Device);
    UNREFERENCED_PARAMETER(Interrupt);
    // Quiesce the source before WDF disconnects this shared interrupt.
    c->Online = FALSE; UartWrite(c, UART_IMSC, 0); (void)UartRead(c, UART_IMSC);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
BOOLEAN UartIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    UART_CONTEXT *c = UartContext(WdfInterruptGetDevice(Interrupt));
    ULONG pending;
    UNREFERENCED_PARAMETER(MessageId);
    if (!c->Online || !c->Open) return FALSE;
    pending = UartRead(c, UART_MIS) & c->Mask;
    if (!pending) return FALSE;
    if (pending & UART_RX_IRQ) UartCapture(c);
    UartWrite(c, UART_ICR, pending);
    if (c->RxArmed && c->Count) c->RxPending = TRUE;
    if (c->TxArmed && !(UartRead(c, UART_FR) & UART_TX_FULL)) {
        c->TxPending = TRUE;
        c->Mask &= ~UART_TX_IRQ; UartWrite(c, UART_IMSC, c->Mask);
    }
    (void)UartRead(c, UART_IMSC);
    if (c->RxPending || c->TxPending || (c->Events & c->WaitMask))
        (void)WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}
_Use_decl_annotations_
VOID UartDpc(WDFINTERRUPT Interrupt, WDFOBJECT Device)
{
    UART_CONTEXT *c = UartContext(Device);
    BOOLEAN rx, tx;
    ULONG events;
    WdfInterruptAcquireLock(Interrupt);
    rx = c->RxArmed && c->RxPending; tx = c->TxArmed && c->TxPending;
    if (rx) c->RxArmed = c->RxPending = FALSE;
    if (tx) c->TxArmed = c->TxPending = FALSE;
    events = c->Events & c->WaitMask; c->Events = 0;
    WdfInterruptReleaseLock(Interrupt);
    // Cancellation returns FALSE once a notification is committed here.
    // Do not hold our lock across a framework notification (it may reenter).
    if (rx) SerCx2PioReceiveReady(c->Receive);
    if (tx) SerCx2PioTransmitReady(c->Transmit);
    if (events) SerCx2CompleteWait(c->Device, events);
}
// The WDK callback type annotates the entire buffer as output, but SerCx2
// explicitly permits a short read and consumes only the returned byte count.
#pragma warning(suppress: 6101)
_Use_decl_annotations_
ULONG UartReceive(SERCX2PIORECEIVE Receive, PUCHAR Buffer, ULONG Length)
{
    UART_CONTEXT *c = UartPioContext(Receive)->Controller;
    ULONG done = 0;
    WdfInterruptAcquireLock(c->Interrupt);
    if (c->Online && c->Open) {
        UartCapture(c);
        while (done < Length && c->Count) {
            Buffer[done++] = c->Ring[c->Tail]; c->Tail = (c->Tail + 1) % UART_RING_SIZE; --c->Count;
        }
    }
    WdfInterruptReleaseLock(c->Interrupt);
    return done;
}
_Use_decl_annotations_
ULONG UartTransmit(SERCX2PIOTRANSMIT Transmit, PUCHAR Buffer, ULONG Length)
{
    UART_CONTEXT *c = UartPioContext(Transmit)->Controller;
    ULONG done = 0;
    WdfInterruptAcquireLock(c->Interrupt);
    if (c->Online && c->Open) {
        while (done < Length && done < 64 && !(UartRead(c, UART_FR) & UART_TX_FULL))
            UartWrite(c, UART_DR, Buffer[done++]);
        (void)UartRead(c, UART_FR);
    }
    WdfInterruptReleaseLock(c->Interrupt);
    return done;
}
_Use_decl_annotations_
VOID UartRxEnable(SERCX2PIORECEIVE Receive)
{
    UART_CONTEXT *c = UartPioContext(Receive)->Controller;
    WdfInterruptAcquireLock(c->Interrupt);
    c->RxArmed = TRUE;
    if (c->Count) { c->RxPending = TRUE; (void)WdfInterruptQueueDpcForIsr(c->Interrupt); }
    WdfInterruptReleaseLock(c->Interrupt);
}
_Use_decl_annotations_
BOOLEAN UartRxCancel(SERCX2PIORECEIVE Receive)
{
    UART_CONTEXT *c = UartPioContext(Receive)->Controller;
    BOOLEAN cancelled;
    WdfInterruptAcquireLock(c->Interrupt);
    cancelled = c->RxArmed; c->RxArmed = c->RxPending = FALSE;
    WdfInterruptReleaseLock(c->Interrupt);
    return cancelled;
}
_Use_decl_annotations_
VOID UartTxEnable(SERCX2PIOTRANSMIT Transmit)
{
    UART_CONTEXT *c = UartPioContext(Transmit)->Controller;
    WdfInterruptAcquireLock(c->Interrupt);
    c->TxArmed = TRUE;
    if (!(UartRead(c, UART_FR) & UART_TX_FULL)) {
        c->TxPending = TRUE; (void)WdfInterruptQueueDpcForIsr(c->Interrupt);
    } else InterruptMask(c);
    WdfInterruptReleaseLock(c->Interrupt);
}
_Use_decl_annotations_
BOOLEAN UartTxCancel(SERCX2PIOTRANSMIT Transmit)
{
    UART_CONTEXT *c = UartPioContext(Transmit)->Controller;
    BOOLEAN cancelled;
    WdfInterruptAcquireLock(c->Interrupt);
    cancelled = c->TxArmed; c->TxArmed = c->TxPending = FALSE; InterruptMask(c);
    WdfInterruptReleaseLock(c->Interrupt);
    return cancelled;
}
_Use_decl_annotations_
VOID UartDrain(SERCX2PIOTRANSMIT Transmit)
{
    UART_CONTEXT *c = UartPioContext(Transmit)->Controller;
    WdfInterruptAcquireLock(c->Interrupt);
    c->Draining = TRUE;
    WdfInterruptReleaseLock(c->Interrupt);
    (void)WdfTimerStart(c->DrainTimer, WDF_REL_TIMEOUT_IN_MS(1));
}
_Use_decl_annotations_
BOOLEAN UartDrainCancel(SERCX2PIOTRANSMIT Transmit)
{
    UART_CONTEXT *c = UartPioContext(Transmit)->Controller;
    BOOLEAN cancelled;
    WdfInterruptAcquireLock(c->Interrupt);
    cancelled = c->Draining; c->Draining = FALSE;
    WdfInterruptReleaseLock(c->Interrupt);
    (void)WdfTimerStop(c->DrainTimer, FALSE);
    return cancelled;
}
_Use_decl_annotations_
VOID UartDrainTimer(WDFTIMER Timer)
{
    UART_CONTEXT *c = UartContext(WdfTimerGetParentObject(Timer));
    BOOLEAN ready = FALSE, again = FALSE, purging = FALSE;
    WdfInterruptAcquireLock(c->Interrupt);
    if ((c->Draining || c->Purging) && c->Online) {
        if (!(UartRead(c, UART_FR) & UART_BUSY)) {
            purging = c->Purging; c->Draining = c->Purging = FALSE; ready = TRUE;
        }
        else again = TRUE;
    }
    WdfInterruptReleaseLock(c->Interrupt);
    if (again) (void)WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(1));
    if (ready) {
        if (purging) SerCx2PioTransmitPurgeFifoComplete(c->Transmit, 0);
        else SerCx2PioTransmitDrainFifoComplete(c->Transmit);
    }
}
_Use_decl_annotations_
VOID UartTxPurge(SERCX2PIOTRANSMIT Transmit, ULONG BytesAlreadyTransmittedToHardware)
{
    UART_CONTEXT *c = UartPioContext(Transmit)->Controller;
    UNREFERENCED_PARAMETER(BytesAlreadyTransmittedToHardware);
    // PL011 has no TX FIFO occupancy counter or selective TX flush. There is
    // no software TX queue here. Let at most 32 hardware bytes finish before
    // reporting zero bytes purged, preserving RX and accurate write counts.
    // Hardware flow control is disabled, so the transmitter always progresses.
    WdfInterruptAcquireLock(c->Interrupt);
    c->Purging = TRUE;
    WdfInterruptReleaseLock(c->Interrupt);
    (void)WdfTimerStart(c->DrainTimer, WDF_REL_TIMEOUT_IN_MS(1));
}
_Use_decl_annotations_
VOID UartPurge(WDFDEVICE Device, BOOLEAN Rx, BOOLEAN Tx)
{
    UART_CONTEXT *c = UartContext(Device);
    WdfInterruptAcquireLock(c->Interrupt);
    UartCapture(c);
    if (Tx) {
        UartWrite(c, UART_CR, 0); UartWrite(c, UART_LCRH, c->Line & ~0x10u);
        UartWrite(c, UART_LCRH, c->Line); UartWrite(c, UART_CR, c->Control);
    }
    if (Rx) c->Head = c->Tail = c->Count = 0;
    UartWrite(c, UART_RSR, 0); UartWrite(c, UART_ICR, 0x7ff);
    (void)UartRead(c, UART_CR);
    WdfInterruptReleaseLock(c->Interrupt);
}
_Use_decl_annotations_
VOID UartWait(WDFDEVICE Device, WDFREQUEST Request, ULONG Mask)
{
    UART_CONTEXT *c = UartContext(Device);
    NTSTATUS status = STATUS_SUCCESS;
    if (Mask & ~(SERIAL_EV_RXCHAR | SERIAL_EV_RXFLAG | SERIAL_EV_TXEMPTY | SERIAL_EV_BREAK | SERIAL_EV_ERR))
        status = STATUS_INVALID_PARAMETER;
    else {
        WdfInterruptAcquireLock(c->Interrupt);
        c->WaitMask = Mask; c->Events = 0;
        WdfInterruptReleaseLock(c->Interrupt);
    }
    WdfRequestComplete(Request, status);
}

_Use_decl_annotations_
NTSTATUS UartControl(WDFDEVICE Device, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    UART_CONTEXT *c = UartContext(Device);
    PVOID input = NULL, output = NULL;
    size_t inputSize = 0, outputSize = 0, information = 0;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG value;
    UNREFERENCED_PARAMETER(OutLength); UNREFERENCED_PARAMETER(InLength);
    switch (Code) {
    case IOCTL_SERIAL_SET_BAUD_RATE: inputSize = sizeof(SERIAL_BAUD_RATE); break;
    case IOCTL_SERIAL_GET_BAUD_RATE: outputSize = sizeof(SERIAL_BAUD_RATE); break;
    case IOCTL_SERIAL_SET_LINE_CONTROL: inputSize = sizeof(SERIAL_LINE_CONTROL); break;
    case IOCTL_SERIAL_GET_LINE_CONTROL: outputSize = sizeof(SERIAL_LINE_CONTROL); break;
    case IOCTL_SERIAL_SET_HANDFLOW: inputSize = sizeof(SERIAL_HANDFLOW); break;
    case IOCTL_SERIAL_GET_HANDFLOW: outputSize = sizeof(SERIAL_HANDFLOW); break;
    case IOCTL_SERIAL_SET_CHARS: inputSize = sizeof(SERIAL_CHARS); break;
    case IOCTL_SERIAL_GET_CHARS: outputSize = sizeof(SERIAL_CHARS); break;
    case IOCTL_SERIAL_GET_PROPERTIES: outputSize = sizeof(SERIAL_COMMPROP); break;
    case IOCTL_SERIAL_GET_COMMSTATUS: outputSize = sizeof(SERIAL_STATUS); break;
    case IOCTL_SERIAL_GET_MODEMSTATUS: case IOCTL_SERIAL_GET_MODEM_CONTROL:
    case IOCTL_SERIAL_GET_DTRRTS: outputSize = sizeof(ULONG); break;
    case IOCTL_SERIAL_SET_MODEM_CONTROL: inputSize = sizeof(ULONG); break;
    case IOCTL_SERIAL_SET_QUEUE_SIZE: inputSize = sizeof(SERIAL_QUEUE_SIZE); break;
    case IOCTL_SERIAL_SET_BREAK_ON: case IOCTL_SERIAL_SET_BREAK_OFF:
    case IOCTL_SERIAL_SET_DTR: case IOCTL_SERIAL_CLR_DTR:
    case IOCTL_SERIAL_SET_RTS: case IOCTL_SERIAL_CLR_RTS: break;
    default: status = STATUS_NOT_SUPPORTED; break;
    }
    // WDF buffer retrieval must occur below DIRQL, before taking the interrupt lock.
    if (NT_SUCCESS(status) && inputSize) status = WdfRequestRetrieveInputBuffer(Request, inputSize, &input, NULL);
    if (NT_SUCCESS(status) && outputSize) status = WdfRequestRetrieveOutputBuffer(Request, outputSize, &output, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return status; }
    WdfInterruptAcquireLock(c->Interrupt);
    if (!c->Open || !c->Online) status = STATUS_DEVICE_NOT_READY;
    else switch (Code) {
    case IOCTL_SERIAL_SET_BAUD_RATE:
        status = UartDivisor(c->ClockHz, ((PSERIAL_BAUD_RATE)input)->BaudRate, &value);
        if (NT_SUCCESS(status) && (UartRead(c, UART_FR) & UART_BUSY)) status = STATUS_DEVICE_BUSY;
        if (NT_SUCCESS(status)) { c->Baud = ((PSERIAL_BAUD_RATE)input)->BaudRate; UartProgram(c); }
        break;
    case IOCTL_SERIAL_GET_BAUD_RATE:
        ((PSERIAL_BAUD_RATE)output)->BaudRate = c->Baud; break;
    case IOCTL_SERIAL_SET_LINE_CONTROL:
        status = UartLine(input, &value);
        if (NT_SUCCESS(status) && (UartRead(c, UART_FR) & UART_BUSY)) status = STATUS_DEVICE_BUSY;
        if (NT_SUCCESS(status)) { c->Line = value; UartProgram(c); }
        break;
    case IOCTL_SERIAL_GET_LINE_CONTROL: {
        PSERIAL_LINE_CONTROL line = output;
        line->WordLength = (UCHAR)(((c->Line >> 5) & 3) + 5);
        line->StopBits = (c->Line & 8) ? STOP_BITS_2 : STOP_BIT_1;
        line->Parity = !(c->Line & 2) ? NO_PARITY : (c->Line & 0x80) ?
            ((c->Line & 4) ? SPACE_PARITY : MARK_PARITY) : ((c->Line & 4) ? EVEN_PARITY : ODD_PARITY);
        break;
    }
    case IOCTL_SERIAL_SET_HANDFLOW: {
        PSERIAL_HANDFLOW flow = input;
        // Only TX/RX are routed. Manual DTR/RTS state is retained, but no
        // hardware handshake or software XON/XOFF is advertised.
        if ((flow->ControlHandShake & ~SERIAL_DTR_CONTROL) || (flow->FlowReplace & ~SERIAL_RTS_CONTROL))
            status = STATUS_INVALID_PARAMETER;
        else {
            c->Flow = *flow;
            c->Control = (c->Control & ~0xc00u) | ((flow->ControlHandShake & SERIAL_DTR_CONTROL) ? 0x400 : 0) |
                ((flow->FlowReplace & SERIAL_RTS_CONTROL) ? 0x800 : 0);
            UartWrite(c, UART_CR, c->Control);
        }
        break;
    }
    case IOCTL_SERIAL_GET_HANDFLOW: *(PSERIAL_HANDFLOW)output = c->Flow; break;
    case IOCTL_SERIAL_SET_CHARS: c->Chars = *(PSERIAL_CHARS)input; break;
    case IOCTL_SERIAL_GET_CHARS: *(PSERIAL_CHARS)output = c->Chars; break;
    case IOCTL_SERIAL_GET_PROPERTIES: {
        PSERIAL_COMMPROP p = output;
        RtlZeroMemory(p, sizeof(*p));
        p->PacketLength = sizeof(*p); p->PacketVersion = 2; p->ServiceMask = SERIAL_SP_SERIALCOMM;
        p->MaxTxQueue = 32; p->MaxRxQueue = UART_RING_SIZE; p->CurrentTxQueue = 32; p->CurrentRxQueue = UART_RING_SIZE;
        p->MaxBaud = SERIAL_BAUD_USER; p->SettableBaud = SERIAL_BAUD_USER;
        p->ProvSubType = SERIAL_SP_RS232;
        p->ProvCapabilities = SERIAL_PCF_TOTALTIMEOUTS | SERIAL_PCF_INTTIMEOUTS;
        p->SettableParams = SERIAL_SP_BAUD | SERIAL_SP_DATABITS | SERIAL_SP_STOPBITS | SERIAL_SP_PARITY;
        p->SettableData = SERIAL_DATABITS_5 | SERIAL_DATABITS_6 | SERIAL_DATABITS_7 | SERIAL_DATABITS_8;
        p->SettableStopParity = SERIAL_STOPBITS_10 | SERIAL_STOPBITS_20 | SERIAL_PARITY_NONE |
            SERIAL_PARITY_ODD | SERIAL_PARITY_EVEN | SERIAL_PARITY_MARK | SERIAL_PARITY_SPACE;
        break;
    }
    case IOCTL_SERIAL_GET_COMMSTATUS: {
        PSERIAL_STATUS p = output;
        RtlZeroMemory(p, sizeof(*p)); p->Errors = c->Errors; c->Errors = 0;
        p->AmountInInQueue = c->Count;
        p->AmountInOutQueue = (UartRead(c, UART_FR) & UART_BUSY) ? 1 : 0;
        break;
    }
    case IOCTL_SERIAL_GET_MODEMSTATUS:
        value = UartRead(c, UART_FR);
        // IOCTL_SERIAL_GET_MODEMSTATUS uses the standard MSR high nibble.
        *(PULONG)output = ((value & 1) ? 0x10u : 0) | ((value & 2) ? 0x20u : 0) |
            ((value & 4) ? 0x80u : 0) | ((value & 0x100) ? 0x40u : 0);
        break;
    case IOCTL_SERIAL_GET_MODEM_CONTROL: case IOCTL_SERIAL_GET_DTRRTS:
        *(PULONG)output = ((c->Control & 0x400) ? SERIAL_DTR_STATE : 0) | ((c->Control & 0x800) ? SERIAL_RTS_STATE : 0);
        break;
    case IOCTL_SERIAL_SET_MODEM_CONTROL:
        value = *(PULONG)input;
        if (value & ~(SERIAL_DTR_STATE | SERIAL_RTS_STATE)) status = STATUS_INVALID_PARAMETER;
        else {
            c->Control = (c->Control & ~0xc00u) | ((value & SERIAL_DTR_STATE) ? 0x400 : 0) |
                ((value & SERIAL_RTS_STATE) ? 0x800 : 0);
            UartWrite(c, UART_CR, c->Control);
        }
        break;
    case IOCTL_SERIAL_SET_DTR: c->Control |= 0x400; UartWrite(c, UART_CR, c->Control); break;
    case IOCTL_SERIAL_CLR_DTR: c->Control &= ~0x400u; UartWrite(c, UART_CR, c->Control); break;
    case IOCTL_SERIAL_SET_RTS: c->Control |= 0x800; UartWrite(c, UART_CR, c->Control); break;
    case IOCTL_SERIAL_CLR_RTS: c->Control &= ~0x800u; UartWrite(c, UART_CR, c->Control); break;
    case IOCTL_SERIAL_SET_BREAK_ON: c->Line |= 1; UartWrite(c, UART_LCRH, c->Line); break;
    case IOCTL_SERIAL_SET_BREAK_OFF: c->Line &= ~1u; UartWrite(c, UART_LCRH, c->Line); break;
    case IOCTL_SERIAL_SET_QUEUE_SIZE: break; // Queue hints do not change the fixed bounded ring.
    default: status = STATUS_NOT_SUPPORTED; break;
    }
    if (NT_SUCCESS(status)) information = outputSize;
    WdfInterruptReleaseLock(c->Interrupt);
    WdfRequestCompleteWithInformation(Request, status, information);
    return status;
}
