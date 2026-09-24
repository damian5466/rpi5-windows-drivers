// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"
#include <acpiioct.h>
#include <reshub.h>

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
EVT_WDF_TIMER UartQuirkTimer;
EVT_WDF_TIMER UartTelemetry;
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
static VOID UartStage(UART_CONTEXT *c, ULONG Stage)
{
#if DBG
    DECLARE_CONST_UNICODE_STRING(name, L"BcmUartStartStage");
    WDFKEY key;
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(c->Device, PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignULong(key, &name, Stage);
        // Persist the last completed startup phase if firmware evaluation fails.
        (void)ZwFlushKey(WdfRegistryWdmGetHandle(key));
        WdfRegistryClose(key);
    }
#else
    UNREFERENCED_PARAMETER(c); UNREFERENCED_PARAMETER(Stage);
#endif
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
    c = UartContext(device); c->Device = device; c->Baud = 115200; c->Line = 3; c->Control = 0x2a;
    UartStage(c, 10);
    WDF_INTERRUPT_CONFIG_INIT(&interrupt, UartIsr, UartDpc);
    interrupt.EvtInterruptEnable = UartInterruptEnable;
    interrupt.EvtInterruptDisable = UartInterruptDisable;
    status = WdfInterruptCreate(device, &interrupt, WDF_NO_OBJECT_ATTRIBUTES, &c->Interrupt);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG_INIT(&timer, UartQuirkTimer);
    timer.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = device;
    status = WdfTimerCreate(&timer, &a, &c->QuirkTimer);
    if (!NT_SUCCESS(status)) return status;
    timer.EvtTimerFunc = UartTelemetry; a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timer, &a, &c->Telemetry);
    if (!NT_SUCCESS(status)) return status;
    SERCX2_CONFIG_INIT(&serial, UartApply, UartControl, UartPurge);
    serial.EvtSerCx2FileOpen = UartOpen; serial.EvtSerCx2FileClose = UartClose;
    serial.EvtSerCx2SetWaitMask = UartWait;
    status = SerCx2InitializeDevice(device, &serial);
    if (!NT_SUCCESS(status)) return status;
    UartStage(c, 20);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, UART_PIO_CONTEXT);
    SERCX2_PIO_RECEIVE_CONFIG_INIT(&rx, UartReceive, UartRxEnable, UartRxCancel);
    status = SerCx2PioReceiveCreate(device, &rx, &a, &c->Receive);
    if (!NT_SUCCESS(status)) return status;
    UartPioContext(c->Receive)->Controller = c;
    SERCX2_PIO_TRANSMIT_CONFIG_INIT(&tx, UartTransmit, UartTxEnable, UartTxCancel);
    // This 8250 aperture has no unsent-byte counter. Use SerCx2's supported
    // FIFO-acceptance completion semantics, without the optional drain/purge
    // trio, so a cancelled write cannot wait forever for a deasserted CTS.
    // The HCI client waits for controller responses before changing baud/reset.
    status = SerCx2PioTransmitCreate(device, &tx, &a, &c->Transmit);
    if (NT_SUCCESS(status)) UartPioContext(c->Transmit)->Controller = c;
    if (NT_SUCCESS(status)) UartStage(c, 30);
    return status;
}
_Use_decl_annotations_
NTSTATUS UartPrepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UART_CONTEXT *c = UartContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    ULONG i, interrupts = 0, pins = 0;
    ACPI_EVAL_INPUT_BUFFER input = {0}; ACPI_EVAL_OUTPUT_BUFFER output = {0};
    WDF_MEMORY_DESCRIPTOR in, out;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Raw);
    UartStage(c, 40);
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeMemory) {
            if (memory || r->u.Memory.Length != 0x20) return STATUS_DEVICE_CONFIGURATION_ERROR;
            memory = r;
        } else if (r->Type == CmResourceTypeInterrupt) {
            if ((r->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED)) ||
                r->ShareDisposition != CmResourceShareDeviceExclusive) return STATUS_DEVICE_CONFIGURATION_ERROR;
            ++interrupts;
        } else if (r->Type == CmResourceTypeConnection &&
            r->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_FUNCTION_CONFIG &&
            r->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_FUNCTION_CONFIG) ++pins;
    }
    // New firmware separates RTS because its alternate function differs on C0.
    if (!memory || interrupts != 1 || (pins != 1 && pins != 2)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE; RtlCopyMemory(input.MethodName, "UCLK", 4);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &input, sizeof(input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &output, sizeof(output));
    UartStage(c, 50);
    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device), NULL,
        IOCTL_ACPI_EVAL_METHOD, &in, &out, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;
    UartStage(c, 60);
    if (output.Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || output.Count != 1 ||
        output.Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER || output.Argument[0].DataLength != 4)
        return STATUS_ACPI_INVALID_DATA;
    c->ClockHz = output.Argument[0].Argument;
    status = UartDivisor(c->ClockHz, 115200, &i);
    if (!NT_SUCCESS(status)) return status;
    c->Registers = MmMapIoSpaceEx(memory->u.Memory.Start, 0x20, PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->Registers) return STATUS_INSUFFICIENT_RESOURCES;
    UartStage(c, 70);
    c->Boot[0] = UartRead(c, UART_LCR);
    if ((c->Boot[0] & 0x80) || UartRead(c, UART_IER)) {
        MmUnmapIoSpace(c->Registers, 0x20); c->Registers = NULL; return STATUS_DEVICE_BUSY;
    }
    c->Boot[1] = UartRead(c, UART_MCR); c->Boot[2] = 0; c->Boot[5] = UartRead(c, UART_SCR);
    UartWrite(c, UART_LCR, c->Boot[0] | 0x80);
    c->Boot[3] = UartRead(c, UART_DLL); c->Boot[4] = UartRead(c, UART_DLM);
    UartWrite(c, UART_LCR, c->Boot[0]);
    status = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES, &c->Key);
    if (!NT_SUCCESS(status)) { MmUnmapIoSpace(c->Registers, 0x20); c->Registers = NULL; return status; }
    UartStage(c, 80);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS UartRelease(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UART_CONTEXT *c = UartContext(Device);
    UNREFERENCED_PARAMETER(Translated);
    (void)WdfTimerStop(c->Telemetry, TRUE);
    if (c->Key) { WdfRegistryClose(c->Key); c->Key = NULL; }
    if (c->Registers) { MmUnmapIoSpace(c->Registers, 0x20); c->Registers = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS UartStart(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    UART_CONTEXT *c = UartContext(Device);
    UNREFERENCED_PARAMETER(Previous);
    UartWrite(c, UART_IER, 0);
    UartWrite(c, UART_FCR, UART_FIFO | 6); // Reset both FIFOs only at startup.
    UartProgram(c);
    UartStage(c, 90);
    (void)WdfTimerStart(c->Telemetry, WDF_REL_TIMEOUT_IN_SEC(1));
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "Pi5BcmUart: BCM7271 UART, clock %lu Hz, 32-byte FIFO\n", c->ClockHz);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS UartStop(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    UART_CONTEXT *c = UartContext(Device);
    UNREFERENCED_PARAMETER(Target);
    WdfInterruptAcquireLock(c->Interrupt);
    c->Online = FALSE; c->Quirk = c->QuirkPending = FALSE;
    UartWrite(c, UART_IER, 0); UartWrite(c, UART_MCR, 0);
    WdfInterruptReleaseLock(c->Interrupt);
    (void)WdfTimerStop(c->QuirkTimer, TRUE);
    (void)WdfTimerStop(c->Telemetry, TRUE);
    UartWrite(c, UART_FCR, 0);
    UartWrite(c, UART_LCR, c->Boot[0] | 0x80);
    UartWrite(c, UART_DLL, c->Boot[3]); UartWrite(c, UART_DLM, c->Boot[4]);
    UartWrite(c, UART_LCR, c->Boot[0]); UartWrite(c, UART_MCR, c->Boot[1]);
    UartWrite(c, UART_SCR, c->Boot[5]); UartWrite(c, UART_IER, c->Boot[2]);
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
        c->RxArmed = c->RxPending = c->TxArmed = c->TxPending = FALSE;
        c->Baud = 115200; c->Line = 3; c->Control = 0x2a;
        RtlZeroMemory(&c->Flow, sizeof(c->Flow)); RtlZeroMemory(&c->Chars, sizeof(c->Chars));
        c->Open = TRUE;
        c->RxPaused = c->Quirk = c->QuirkPending = FALSE;
        c->Flow.ControlHandShake = SERIAL_CTS_HANDSHAKE; c->Flow.FlowReplace = SERIAL_RTS_HANDSHAKE;
        // Start a new session from a disabled FIFO, as on a fresh D0 entry.
        UartWrite(c, UART_IER, 0);
        UartWrite(c, UART_FCR, 0);
        UartWrite(c, UART_FCR, UART_FIFO | 6); UartProgram(c);
    }
    WdfInterruptReleaseLock(c->Interrupt);
    return status;
}
_Use_decl_annotations_
VOID UartClose(WDFDEVICE Device)
{
    UART_CONTEXT *c = UartContext(Device);
    WdfInterruptAcquireLock(c->Interrupt);
    c->Open = FALSE; FALSE;
    c->Quirk = c->QuirkPending = FALSE; UartInterruptMask(c);
    c->Head = c->Tail = c->Count = c->WaitMask = c->Events = 0;
    WdfInterruptReleaseLock(c->Interrupt);
    (void)WdfTimerStop(c->QuirkTimer, FALSE);
}
_Use_decl_annotations_
NTSTATUS UartApply(WDFDEVICE Device, PVOID Parameters)
{
    UART_CONTEXT *c = UartContext(Device);
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER properties = Parameters;
    // ACPI UART serial-bus descriptor, packed per the ACPI specification.
#pragma pack(push, 1)
    typedef struct { PNP_SERIAL_BUS_DESCRIPTOR Serial; ULONG Baud; USHORT Rx, Tx; UCHAR Parity, Lines; } UART_DESCRIPTOR;
#pragma pack(pop)
    const UART_DESCRIPTOR *d;
    ULONG divisor;
    NTSTATUS status;
    if (!properties || properties->PropertiesLength < sizeof(*d)) return STATUS_INVALID_PARAMETER;
    d = (const UART_DESCRIPTOR *)properties->ConnectionProperties;
    // 8N1, hardware flow control, little endian; only CTS and RTS are routed.
    if (d->Serial.SerialBusType != 3 || d->Serial.TypeSpecificFlags != 0x35 || d->Parity ||
        d->Lines != 0xc0) return STATUS_NOT_SUPPORTED;
    status = UartDivisor(c->ClockHz, d->Baud, &divisor);
    if (!NT_SUCCESS(status)) return status;
    WdfInterruptAcquireLock(c->Interrupt);
    c->Baud = d->Baud; c->Line = 3; c->Control = 0x2a;
    c->Flow.ControlHandShake = SERIAL_CTS_HANDSHAKE; c->Flow.FlowReplace = SERIAL_RTS_HANDSHAKE;
    UartProgram(c);
    WdfInterruptReleaseLock(c->Interrupt);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS UartInterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    UART_CONTEXT *c = UartContext(Device);
    UNREFERENCED_PARAMETER(Interrupt);
    c->Online = TRUE; UartInterruptMask(c);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS UartInterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    UART_CONTEXT *c = UartContext(Device);
    UNREFERENCED_PARAMETER(Interrupt);
    // Quiesce the source before WDF disconnects this shared interrupt.
    c->Online = FALSE; UartWrite(c, UART_IER, 0); (void)UartRead(c, UART_IER);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
BOOLEAN UartIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    UART_CONTEXT *c = UartContext(WdfInterruptGetDevice(Interrupt));
    ULONG pending, i; BOOLEAN handled = FALSE;
    UNREFERENCED_PARAMETER(MessageId);
    if (!c->Online || !c->Open) return FALSE;
    for (i = 0; i < 8; ++i) {
        pending = UartRead(c, UART_IIR);
        if (pending & 1) break;
        UartTrace(c, 3, pending, c->Count);
        handled = TRUE;
        if ((pending & 14) == 12 && !(UartRead(c, UART_LSR) & UART_RX_READY)) {
            // BCM7271 empty receive-timeout erratum: stop the peer before
            // reading a dummy byte, so an in-flight H4 byte cannot be lost.
            ++c->Quirks; c->Quirk = c->QuirkPending = TRUE; UartInterruptMask(c); break;
        }
        if ((pending & 14) == 4 || (pending & 14) == 6 || (pending & 14) == 12) UartCapture(c);
        else if ((pending & 14) == 2) {
            if (c->TxArmed) c->TxPending = TRUE;
            c->Mask &= ~UART_TX_IRQ; UartWrite(c, UART_IER, c->Mask);
        } else if ((pending & 14) == 0) {
            if (UartRead(c, UART_MSR) & 1) c->Events |= SERIAL_EV_CTS;
        } else break;
    }
    if (c->RxArmed && c->Count) c->RxPending = TRUE;
    if (handled) ++c->Irqs;
    if (c->RxPending || c->TxPending || c->QuirkPending || (c->Events & c->WaitMask))
        (void)WdfInterruptQueueDpcForIsr(Interrupt);
    return handled;
}
_Use_decl_annotations_
VOID UartDpc(WDFINTERRUPT Interrupt, WDFOBJECT Device)
{
    UART_CONTEXT *c = UartContext(Device);
    BOOLEAN rx, tx, quirk;
    ULONG quirkDelay;
    ULONG events;
    WdfInterruptAcquireLock(Interrupt);
    rx = c->RxArmed && c->RxPending; tx = c->TxArmed && c->TxPending;
    if (rx) c->RxArmed = c->RxPending = FALSE;
    if (tx) c->TxArmed = c->TxPending = FALSE;
    events = c->Events & c->WaitMask; c->Events = 0;
    quirk = c->QuirkPending; c->QuirkPending = FALSE; quirkDelay = (30000 + c->Baud - 1) / c->Baud;
    WdfInterruptReleaseLock(Interrupt);
    // Cancellation returns FALSE once a notification is committed here.
    // Do not hold our lock across a framework notification (it may reenter).
    if (quirk) (void)WdfTimerStart(c->QuirkTimer, WDF_REL_TIMEOUT_IN_MS(quirkDelay));
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
        if (c->RxPaused && c->Count < UART_LOW_WATER) { c->RxPaused = FALSE; UartInterruptMask(c); }
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
        ULONG line = UartRead(c, UART_LSR);
        UartTrace(c, 4, line, Length);
        if (line & UART_TX_EMPTY)
            while (done < Length && done < 32) UartWrite(c, UART_THR, Buffer[done++]);
        (void)UartRead(c, UART_IER);
        c->TxBytes += done;
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
    if ((UartRead(c, UART_LSR) & UART_TX_EMPTY)) {
        c->TxPending = TRUE; (void)WdfInterruptQueueDpcForIsr(c->Interrupt);
    } else UartInterruptMask(c);
    WdfInterruptReleaseLock(c->Interrupt);
}
_Use_decl_annotations_
BOOLEAN UartTxCancel(SERCX2PIOTRANSMIT Transmit)
{
    UART_CONTEXT *c = UartPioContext(Transmit)->Controller;
    BOOLEAN cancelled;
    WdfInterruptAcquireLock(c->Interrupt);
    cancelled = c->TxArmed; c->TxArmed = c->TxPending = FALSE; UartInterruptMask(c);
    WdfInterruptReleaseLock(c->Interrupt);
    return cancelled;
}
_Use_decl_annotations_
VOID UartPurge(WDFDEVICE Device, BOOLEAN Rx, BOOLEAN Tx)
{
    UART_CONTEXT *c = UartContext(Device);
    WdfInterruptAcquireLock(c->Interrupt);
    UartWrite(c, UART_FCR, UART_FIFO | (Rx ? 2 : 0) | (Tx ? 4 : 0));
    if (Rx) { c->Head = c->Tail = c->Count = 0; c->RxPaused = FALSE; }
    UartInterruptMask(c);
    WdfInterruptReleaseLock(c->Interrupt);
}
_Use_decl_annotations_
VOID UartWait(WDFDEVICE Device, WDFREQUEST Request, ULONG Mask)
{
    UART_CONTEXT *c = UartContext(Device);
    NTSTATUS status = STATUS_SUCCESS;
    if (Mask & ~(SERIAL_EV_RXCHAR | SERIAL_EV_RXFLAG | SERIAL_EV_TXEMPTY | SERIAL_EV_BREAK | SERIAL_EV_ERR | SERIAL_EV_CTS))
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
        if (NT_SUCCESS(status) && !(UartRead(c, UART_LSR) & UART_IDLE)) status = STATUS_DEVICE_BUSY;
        if (NT_SUCCESS(status)) { c->Baud = ((PSERIAL_BAUD_RATE)input)->BaudRate; UartProgram(c); }
        break;
    case IOCTL_SERIAL_GET_BAUD_RATE:
        ((PSERIAL_BAUD_RATE)output)->BaudRate = c->Baud; break;
    case IOCTL_SERIAL_SET_LINE_CONTROL:
        status = UartLine(input, &value);
        if (NT_SUCCESS(status) && !(UartRead(c, UART_LSR) & UART_IDLE)) status = STATUS_DEVICE_BUSY;
        if (NT_SUCCESS(status)) { c->Line = value; UartProgram(c); }
        break;
    case IOCTL_SERIAL_GET_LINE_CONTROL: {
        PSERIAL_LINE_CONTROL line = output;
        line->WordLength = (UCHAR)((c->Line & 3) + 5);
        line->StopBits = (c->Line & 4) ? STOP_BITS_2 : STOP_BIT_1;
        line->Parity = !(c->Line & 8) ? NO_PARITY : (c->Line & 0x20) ?
            ((c->Line & 0x10) ? SPACE_PARITY : MARK_PARITY) : ((c->Line & 0x10) ? EVEN_PARITY : ODD_PARITY);
        break;
    }
    case IOCTL_SERIAL_SET_HANDFLOW: {
        PSERIAL_HANDFLOW flow = input;
        if (flow->ControlHandShake != SERIAL_CTS_HANDSHAKE ||
            (flow->FlowReplace != 0 && flow->FlowReplace != SERIAL_RTS_CONTROL &&
             flow->FlowReplace != SERIAL_RTS_HANDSHAKE))
            status = STATUS_NOT_SUPPORTED;
        else {
            // CTS remains automatic; RTS may be held deasserted while a
            // Bluetooth client resets its controller (normal boot strap).
            c->Flow = *flow; c->Control |= UART_AFE;
            if (flow->FlowReplace) c->Control |= UART_RTS;
            else c->Control &= ~UART_RTS;
            UartInterruptMask(c);
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
        p->ProvCapabilities = SERIAL_PCF_TOTALTIMEOUTS | SERIAL_PCF_INTTIMEOUTS | SERIAL_PCF_RTSCTS;
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
        p->AmountInOutQueue = !(UartRead(c, UART_LSR) & UART_IDLE) ? 1 : 0;
#if DBG
        UartTrace(c, 5, UartRead(c, UART_LSR), UartRead(c, UART_IIR));
#endif
        break;
    }
    case IOCTL_SERIAL_GET_MODEMSTATUS: *(PULONG)output = UartRead(c, UART_MSR) & 0xf0; break;
    case IOCTL_SERIAL_GET_MODEM_CONTROL: case IOCTL_SERIAL_GET_DTRRTS:
        *(PULONG)output = (c->Control & UART_RTS) ? SERIAL_RTS_STATE : 0; break;
    case IOCTL_SERIAL_SET_BREAK_ON: c->Line |= 0x40; UartWrite(c, UART_LCR, c->Line); break;
    case IOCTL_SERIAL_SET_BREAK_OFF: c->Line &= ~0x40u; UartWrite(c, UART_LCR, c->Line); break;
    case IOCTL_SERIAL_SET_MODEM_CONTROL: case IOCTL_SERIAL_SET_DTR: case IOCTL_SERIAL_CLR_DTR:
    case IOCTL_SERIAL_SET_RTS: case IOCTL_SERIAL_CLR_RTS: status = STATUS_NOT_SUPPORTED; break;
    case IOCTL_SERIAL_SET_QUEUE_SIZE: break; // Queue hints do not change the fixed bounded ring.
    default: status = STATUS_NOT_SUPPORTED; break;
    }
    if (NT_SUCCESS(status)) information = outputSize;
    c->LastIoctl = Code; c->LastStatus = status;
    WdfInterruptReleaseLock(c->Interrupt);
    WdfRequestCompleteWithInformation(Request, status, information);
    return status;
}

_Use_decl_annotations_
VOID UartQuirkTimer(WDFTIMER Timer)
{
    UART_CONTEXT *c = UartContext(WdfTimerGetParentObject(Timer));
    WdfInterruptAcquireLock(c->Interrupt);
    if (c->Online && c->Open && c->Quirk) {
        if (!(UartRead(c, UART_LSR) & UART_RX_READY)) (void)UartRead(c, UART_RBR);
        else UartCapture(c);
        c->Quirk = FALSE; UartInterruptMask(c);
        if (c->RxArmed && c->Count) {
            c->RxPending = TRUE; (void)WdfInterruptQueueDpcForIsr(c->Interrupt);
        }
    }
    WdfInterruptReleaseLock(c->Interrupt);
}

_Use_decl_annotations_
VOID UartTelemetry(WDFTIMER Timer)
{
    UART_CONTEXT *c = UartContext(WdfTimerGetParentObject(Timer));
    ULONG values[14];
#if DBG
    ULONG trace[1 + 128 * 3];
    UNICODE_STRING traceName = RTL_CONSTANT_STRING(L"BcmUartTrace");
#endif
    BOOLEAN online;
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"BcmUartDiagnostics");
    WdfInterruptAcquireLock(c->Interrupt);
    values[0] = c->ClockHz; values[1] = c->Baud; values[2] = c->Line; values[3] = c->Control;
    values[4] = c->Mask; values[5] = c->Open; values[6] = c->Count; values[7] = c->Errors;
    values[8] = c->Irqs; values[9] = c->RxBytes; values[10] = c->TxBytes; values[11] = c->Quirks;
    values[12] = c->LastIoctl; values[13] = c->LastStatus; online = c->Online;
#if DBG
    trace[0] = c->TraceCount;
    RtlCopyMemory(trace + 1, c->Trace, sizeof(c->Trace));
#endif
    WdfInterruptReleaseLock(c->Interrupt);
    if (c->Key) (void)WdfRegistryAssignValue(c->Key, &name, REG_BINARY, sizeof(values), values);
#if DBG
    if (c->Key) (void)WdfRegistryAssignValue(c->Key, &traceName, REG_BINARY, sizeof(trace), trace);
#endif
    if (online) (void)WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_SEC(1));
}
