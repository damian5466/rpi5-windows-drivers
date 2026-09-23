// SPDX-License-Identifier: BSD-2-Clause-Patent
#define _NO_CRT_STDIO_INLINE
#include "hardware.h"
#include <wdf.h>
#include <spbcx.h>
#include <ntstrsafe.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <gpio.h>
#define RP1_SERVICE_CLIENT
#define RP1_CLOCK_CLIENT
#include "rp1-service.h"
#include "rp1-clock.h"
#include "rp1-acpi.h"

typedef struct {
    WDFDEVICE Device;
    WDFINTERRUPT Interrupt;
    WDFWORKITEM Worker;
    WDFSPINLOCK Lock;
    WDFIOTARGET Route, Clock;
    WDFREQUEST Request;
    KEVENT Wake;
    PUCHAR Registers, Buffer;
    volatile LONG Online;
    ULONG Uid, ClockHz, Depth, FrameControl, Boot[11];
    LARGE_INTEGER ChipSelect[2];
} SPI_CONTEXT;
typedef struct { ULONG Select, Speed, Mode; WDFIOTARGET Pins; } SPI_TARGET;
typedef struct { ULONG Offset, Length; BOOLEAN Read; } SPI_TRANSFER;
typedef struct {
    SPI_CONTEXT *Controller;
    SPI_TARGET Target;
    SPI_ENGINE Engine;
    PMDL Mdls[SPI_MAX_TRANSFERS];
    SPI_TRANSFER Transfers[SPI_MAX_TRANSFERS];
    ULONG Total, Count;
    volatile LONG Cancelled, Completion;
    NTSTATUS Status;
} SPI_REQUEST;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SPI_CONTEXT, SpiContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SPI_TARGET, SpiTarget)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SPI_REQUEST, SpiRequest)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SpiAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SpiPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE SpiRelease;
EVT_WDF_DEVICE_D0_ENTRY SpiStart;
EVT_WDF_DEVICE_D0_EXIT SpiStop;
EVT_WDF_INTERRUPT_ISR SpiIsr;
EVT_WDF_INTERRUPT_DPC SpiDpc;
EVT_WDF_INTERRUPT_ENABLE SpiInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE SpiInterruptDisable;
EVT_WDF_WORKITEM SpiWorker;
EVT_WDF_REQUEST_CANCEL SpiCancel;
EVT_SPB_TARGET_CONNECT SpiConnect;
EVT_SPB_TARGET_DISCONNECT SpiDisconnect;
EVT_SPB_CONTROLLER_OTHER SpiOther;
EVT_WDF_IO_IN_CALLER_CONTEXT SpiCaller;
EVT_SPB_CONTROLLER_READ SpiReadRequest;
EVT_SPB_CONTROLLER_WRITE SpiWriteRequest;
EVT_SPB_CONTROLLER_SEQUENCE SpiSequence;
static const ULONG SavedOffsets[] = { SPI_CONTROL, SPI_COUNT, SPI_SELECT, SPI_DIVIDER,
    SPI_TX_THRESHOLD, SPI_RX_THRESHOLD, SPI_MASK, SPI_DMA, 0x50, 0x54, SPI_RX_DELAY };

static SPI_ENGINE Registers(SPI_CONTEXT *c)
{
    SPI_ENGINE e = {0}; e.Registers = c->Registers; return e;
}
static VOID Mask(SPI_CONTEXT *c, ULONG mask)
{
    WRITE_REGISTER_ULONG((PULONG)(c->Registers + SPI_MASK), mask);
    (void)READ_REGISTER_ULONG((PULONG)(c->Registers + SPI_MASK));
}
static VOID Disable(SPI_ENGINE *e)
{
    SpiWrite(e, SPI_ENABLE, 0);
    (void)SpiRead(e, SPI_ENABLE);
    SpiWrite(e, SPI_SELECT, 0);
    (void)SpiRead(e, SPI_CLEAR);
}
static NTSTATUS Select(SPI_TARGET *t, BOOLEAN Active)
{
    UCHAR value = Active ? 0 : 1;
    WDF_MEMORY_DESCRIPTOR buffer;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&buffer, &value, sizeof(value));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(1));
    return WdfIoTargetSendIoctlSynchronously(t->Pins, NULL, IOCTL_GPIO_WRITE_PINS,
        &buffer, &buffer, &options, NULL);
}
_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT Object, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, SpiAdd);
    return WdfDriverCreate(Object, Path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS SpiAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_INTERRUPT_CONFIG interrupt;
    WDF_WORKITEM_CONFIG work;
    SPB_CONTROLLER_CONFIG spb;
    WDFDEVICE device;
    WDFMEMORY memory;
    SPI_CONTEXT *c;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);
    status = SpbDeviceInitConfig(Init);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = SpiPrepare; pnp.EvtDeviceReleaseHardware = SpiRelease;
    pnp.EvtDeviceD0Entry = SpiStart; pnp.EvtDeviceD0Exit = SpiStop;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, SPI_CONTEXT);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&Init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    c = SpiContext(device); c->Device = device;
    KeInitializeEvent(&c->Wake, NotificationEvent, FALSE);
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = device;
    // One bounded buffer per controller avoids allocation failures part-way
    // through a transaction. SpbCx dispatches this controller sequentially.
    status = WdfMemoryCreate(&a, NonPagedPoolNx, 'S1pR', 2 * SPI_MAX_BYTES, &memory, (PVOID *)&c->Buffer);
    if (!NT_SUCCESS(status)) return status;
    status = WdfSpinLockCreate(&a, &c->Lock);
    if (!NT_SUCCESS(status)) return status;
    WDF_INTERRUPT_CONFIG_INIT(&interrupt, SpiIsr, SpiDpc);
    interrupt.AutomaticSerialization = FALSE;
    interrupt.EvtInterruptEnable = SpiInterruptEnable;
    interrupt.EvtInterruptDisable = SpiInterruptDisable;
    status = WdfInterruptCreate(device, &interrupt, WDF_NO_OBJECT_ATTRIBUTES, &c->Interrupt);
    if (!NT_SUCCESS(status)) return status;
    WDF_WORKITEM_CONFIG_INIT(&work, SpiWorker); work.AutomaticSerialization = FALSE;
    status = WdfWorkItemCreate(&work, &a, &c->Worker);
    if (!NT_SUCCESS(status)) return status;
    SPB_CONTROLLER_CONFIG_INIT(&spb);
    spb.EvtSpbTargetConnect = SpiConnect; spb.EvtSpbTargetDisconnect = SpiDisconnect;
    spb.EvtSpbIoRead = SpiReadRequest; spb.EvtSpbIoWrite = SpiWriteRequest;
    spb.EvtSpbIoSequence = SpiSequence;
    status = SpbDeviceInitialize(device, &spb);
    if (!NT_SUCCESS(status)) return status;
    SpbControllerSetIoOtherCallback(device, SpiOther, SpiCaller);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, SPI_TARGET);
    SpbControllerSetTargetAttributes(device, &a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, SPI_REQUEST);
    SpbControllerSetRequestAttributes(device, &a);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS SpiPrepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    SPI_CONTEXT *c = SpiContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    ULONG i, interrupts = 0, pins = 0, gpio = 0;
    NTSTATUS status;
    SPI_ENGINE e;
    UNREFERENCED_PARAMETER(Raw);
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeMemory) {
            if (memory || r->u.Memory.Length != 0x130) return STATUS_DEVICE_CONFIGURATION_ERROR;
            memory = r;
        } else if (r->Type == CmResourceTypeInterrupt) {
            if ((r->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED)) ||
                r->ShareDisposition != CmResourceShareShared) return STATUS_DEVICE_CONFIGURATION_ERROR;
            ++interrupts;
        } else if (r->Type == CmResourceTypeConnection &&
            r->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_FUNCTION_CONFIG &&
            r->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_FUNCTION_CONFIG) ++pins;
        else if (r->Type == CmResourceTypeConnection &&
            r->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
            r->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO) {
            if (gpio == 2) return STATUS_DEVICE_CONFIGURATION_ERROR;
            c->ChipSelect[gpio].LowPart = r->u.Connection.IdLowPart;
            c->ChipSelect[gpio++].HighPart = r->u.Connection.IdHighPart;
        }
    }
    if (!memory || interrupts != 1 || pins != 1 || gpio != 2) return STATUS_DEVICE_CONFIGURATION_ERROR;
    status = Rp1GetUid(Device, &c->Uid);
    if (!NT_SUCCESS(status)) return status;
    if (c->Uid != 0) return STATUS_NOT_SUPPORTED;
    c->Registers = MmMapIoSpaceEx(memory->u.Memory.Start, 0x130, PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->Registers) return STATUS_INSUFFICIENT_RESOURCES;
    e = Registers(c);
    if (SpiRead(&e, SPI_VERSION) != 0x3430322a || SpiRead(&e, SPI_ENABLE) || SpiRead(&e, SPI_DMA)) {
        MmUnmapIoSpace(c->Registers, 0x130); c->Registers = NULL;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    for (i = 0; i < RTL_NUMBER_OF(SavedOffsets); ++i) c->Boot[i] = SpiRead(&e, SavedOffsets[i]);
    status = SpiFrameControl(&e, &c->FrameControl);
    if (!NT_SUCCESS(status)) {
        MmUnmapIoSpace(c->Registers, 0x130); c->Registers = NULL; return status;
    }
    // Threshold is writable only up to FIFO depth minus one. Probe only while
    // disabled, then immediately restore the firmware's threshold.
    for (i = 1; i < 256; ++i) {
        SpiWrite(&e, SPI_TX_THRESHOLD, i);
        if (SpiRead(&e, SPI_TX_THRESHOLD) != i) break;
    }
    c->Depth = i; SpiWrite(&e, SPI_TX_THRESHOLD, c->Boot[4]);
    if (c->Depth < 2) {
        MmUnmapIoSpace(c->Registers, 0x130); c->Registers = NULL; return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS SpiRelease(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    SPI_CONTEXT *c = SpiContext(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (c->Registers) { MmUnmapIoSpace(c->Registers, 0x130); c->Registers = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS SpiStart(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    SPI_CONTEXT *c = SpiContext(Device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Previous);
    status = Rp1OpenClock(Device, FALSE, &c->Clock, &c->ClockHz);
    if (!NT_SUCCESS(status)) return status;
    Mask(c, 0);
    status = Rp1OpenInterruptRoute(Device, 19, &c->Route);
    if (!NT_SUCCESS(status)) { Mask(c, c->Boot[6]); WdfObjectDelete(c->Clock); c->Clock = NULL; }
    return status;
}
_Use_decl_annotations_
NTSTATUS SpiStop(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    SPI_CONTEXT *c = SpiContext(Device);
    SPI_ENGINE e = Registers(c);
    ULONG i;
    UNREFERENCED_PARAMETER(Target);
    InterlockedExchange(&c->Online, 0); KeSetEvent(&c->Wake, IO_NO_INCREMENT, FALSE);
    WdfWorkItemFlush(c->Worker);
    Mask(c, 0);
    Disable(&e);
    for (i = 0; i < RTL_NUMBER_OF(SavedOffsets); ++i)
        if (SavedOffsets[i] != SPI_MASK) SpiWrite(&e, SavedOffsets[i], c->Boot[i]);
    if (c->Route) { WdfObjectDelete(c->Route); c->Route = NULL; }
    Mask(c, c->Boot[6]);
    if (c->Clock) { WdfObjectDelete(c->Clock); c->Clock = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS SpiInterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Interrupt);
    InterlockedExchange(&SpiContext(Device)->Online, 1);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS SpiInterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    SPI_CONTEXT *c = SpiContext(Device);
    InterlockedExchange(&c->Online, 0); Mask(c, 0);
    (void)WdfInterruptQueueDpcForIsr(Interrupt);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
BOOLEAN SpiIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    SPI_CONTEXT *c = SpiContext(WdfInterruptGetDevice(Interrupt));
    ULONG pending;
    UNREFERENCED_PARAMETER(MessageId);
    if (!c->Online) return FALSE;
    pending = READ_REGISTER_ULONG((PULONG)(c->Registers + SPI_INTR));
    if (!pending) return FALSE;
    Mask(c, 0);
    (void)WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}
_Use_decl_annotations_
VOID SpiDpc(WDFINTERRUPT Interrupt, WDFOBJECT Device)
{
    UNREFERENCED_PARAMETER(Interrupt);
    KeSetEvent(&SpiContext(Device)->Wake, IO_NO_INCREMENT, FALSE);
}

#include <pshpack1.h>
typedef struct {
    PNP_SERIAL_BUS_DESCRIPTOR Bus;
    ULONG Speed;
    UCHAR Bits, Phase, Polarity;
    USHORT Select;
} SPI_DESCRIPTOR;
#include <poppack.h>
_Use_decl_annotations_
NTSTATUS SpiConnect(WDFDEVICE Controller, SPBTARGET Target)
{
    SPI_CONTEXT *c = SpiContext(Controller);
    SPB_CONNECTION_PARAMETERS p;
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER properties;
    SPI_DESCRIPTOR *d;
    SPI_TARGET *t = SpiTarget(Target);
    WDF_OBJECT_ATTRIBUTES a;
    WDF_IO_TARGET_OPEN_PARAMS open;
    WCHAR path[RESOURCE_HUB_PATH_SIZE / sizeof(WCHAR)];
    UNICODE_STRING name;
    NTSTATUS status;
    SPB_CONNECTION_PARAMETERS_INIT(&p);
    SpbTargetGetConnectionParameters(Target, &p);
    properties = p.ConnectionParameters;
    if (!properties || properties->PropertiesLength < sizeof(*d)) return STATUS_INVALID_PARAMETER;
    d = (SPI_DESCRIPTOR *)properties->ConnectionProperties;
    if (d->Bus.SerialBusType != 2 || (d->Bus.GeneralFlags & 1) ||
        (d->Bus.TypeSpecificFlags & 3) || d->Bus.TypeDataLength < 9 ||
        d->Select > 1 || d->Bits != 8 || d->Phase > 1 || d->Polarity > 1 ||
        d->Speed < 100000 || d->Speed > 4000000) return STATUS_NOT_SUPPORTED;
    t->Select = d->Select; t->Speed = d->Speed; t->Mode = d->Phase | (d->Polarity << 1);
    RtlInitEmptyUnicodeString(&name, path, sizeof(path));
    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&name, c->ChipSelect[t->Select].LowPart, c->ChipSelect[t->Select].HighPart);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = Controller;
    status = WdfIoTargetCreate(Controller, &a, &t->Pins);
    if (!NT_SUCCESS(status)) return status;
    // PullUp plus OutputOnly primes the inactive level before GPIO output
    // enable. The other chip select is left untouched until its target opens.
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_WRITE);
    status = WdfIoTargetOpen(t->Pins, &open);
    if (NT_SUCCESS(status)) status = Select(t, FALSE);
    if (!NT_SUCCESS(status)) { WdfObjectDelete(t->Pins); t->Pins = NULL; }
    return status;
}
_Use_decl_annotations_
VOID SpiDisconnect(WDFDEVICE Controller, SPBTARGET Target)
{
    SPI_TARGET *t = SpiTarget(Target);
    UNREFERENCED_PARAMETER(Controller);
    if (t->Pins) { (void)Select(t, FALSE); WdfObjectDelete(t->Pins); t->Pins = NULL; }
}

// One reference keeps the context alive until the last cancellation participant
// has handed off completion. Bit 0 means cancel has exited; bit 1 means the
// worker has stopped touching hardware and published its result.
static VOID Complete(WDFREQUEST Request)
{
    SPI_REQUEST *r = SpiRequest(Request);
    NTSTATUS status = r->Cancelled ? STATUS_CANCELLED : r->Status;
    WdfRequestSetInformation(Request, NT_SUCCESS(status) ? r->Total : 0);
    SpbRequestComplete((SPBREQUEST)Request, status);
    WdfObjectDereference(Request);
}
_Use_decl_annotations_
VOID SpiCancel(WDFREQUEST Request)
{
    SPI_REQUEST *r = SpiRequest(Request);
    InterlockedExchange(&r->Cancelled, 1);
    KeSetEvent(&r->Controller->Wake, IO_NO_INCREMENT, FALSE);
    if (InterlockedOr(&r->Completion, 1) & 2) Complete(Request);
}
static VOID Begin(WDFDEVICE Device, SPBTARGET Target, SPBREQUEST Request, ULONG Count, BOOLEAN FullDuplex)
{
    SPI_CONTEXT *c = SpiContext(Device);
    SPI_REQUEST *r = SpiRequest(Request);
    SPB_REQUEST_PARAMETERS p;
    ULONG i;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    SPB_REQUEST_PARAMETERS_INIT(&p);
    SpbRequestGetParameters(Request, &p);
    // A transfer sequence must arrive as one SPB request. Explicit controller
    // locks spanning multiple requests are not supported.
    if ((!FullDuplex && p.Position != SpbRequestSequencePositionSingle) || !Count || Count > SPI_MAX_TRANSFERS || (FullDuplex && Count != 2)) goto fail;
    RtlZeroMemory(r, sizeof(*r));
    r->Controller = c; r->Target = *SpiTarget(Target);
    r->Engine.Registers = c->Registers;
    r->Engine.Tx = c->Buffer; r->Engine.Rx = c->Buffer + SPI_MAX_BYTES;
    r->Engine.Depth = c->Depth; r->Count = Count;
    for (i = 0; i < Count; ++i) {
        SPB_TRANSFER_DESCRIPTOR d;
        SPI_TRANSFER *t = &r->Transfers[i];
        SPB_TRANSFER_DESCRIPTOR_INIT(&d);
        SpbRequestGetTransferParameters(Request, i, &d, &r->Mdls[i]);
        if (!d.TransferLength || d.TransferLength > SPI_MAX_BYTES - r->Total ||
            !r->Mdls[i] || d.DelayInUs ||
            (d.Direction != SpbTransferDirectionFromDevice && d.Direction != SpbTransferDirectionToDevice)) goto fail;
        t->Read = d.Direction == SpbTransferDirectionFromDevice;
        t->Offset = FullDuplex ? 0 : r->Total; t->Length = (ULONG)d.TransferLength;
        if (FullDuplex && t->Read != (i == 1)) goto fail;
        r->Total += t->Length;
        if (FullDuplex) { if (t->Length > r->Engine.Frames) r->Engine.Frames = t->Length; }
        else r->Engine.Frames = r->Total;
    }
    WdfSpinLockAcquire(c->Lock);
    if (c->Request || !c->Online) {
        WdfSpinLockRelease(c->Lock); status = STATUS_DEVICE_NOT_READY; goto fail;
    }
    c->Request = (WDFREQUEST)Request;
    WdfObjectReference(Request);
    WdfSpinLockRelease(c->Lock);
    status = WdfRequestMarkCancelableEx((WDFREQUEST)Request, SpiCancel);
    if (!NT_SUCCESS(status)) {
        WdfSpinLockAcquire(c->Lock); c->Request = NULL; WdfSpinLockRelease(c->Lock);
        r->Status = status; Complete((WDFREQUEST)Request); return;
    }
    WdfWorkItemEnqueue(c->Worker);
    return;
fail:
    SpbRequestComplete(Request, status);
}
_Use_decl_annotations_
VOID SpiReadRequest(WDFDEVICE Device, SPBTARGET Target, SPBREQUEST Request, size_t Length)
{ UNREFERENCED_PARAMETER(Length); Begin(Device, Target, Request, 1, FALSE); }
_Use_decl_annotations_
VOID SpiWriteRequest(WDFDEVICE Device, SPBTARGET Target, SPBREQUEST Request, size_t Length)
{ UNREFERENCED_PARAMETER(Length); Begin(Device, Target, Request, 1, FALSE); }
_Use_decl_annotations_
VOID SpiSequence(WDFDEVICE Device, SPBTARGET Target, SPBREQUEST Request, ULONG Count)
{ Begin(Device, Target, Request, Count, FALSE); }

static NTSTATUS CopyBuffers(SPI_REQUEST *r, BOOLEAN Read)
{
    ULONG i;
    for (i = 0; i < r->Count; ++i) {
        SPI_TRANSFER *t = &r->Transfers[i];
        PMDL mdl;
        ULONG copied = 0;
        if (t->Read != Read) continue;
        for (mdl = r->Mdls[i]; mdl && copied < t->Length; mdl = mdl->Next) {
            ULONG bytes = MmGetMdlByteCount(mdl);
            PUCHAR mapped = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
            if (!mapped) return STATUS_INSUFFICIENT_RESOURCES;
            if (bytes > t->Length - copied) bytes = t->Length - copied;
            if (Read) RtlCopyMemory(mapped, r->Engine.Rx + t->Offset + copied, bytes);
            else RtlCopyMemory(r->Engine.Tx + t->Offset + copied, mapped, bytes);
            copied += bytes;
        }
        if (copied != t->Length) return STATUS_INVALID_BUFFER_SIZE;
    }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
VOID SpiWorker(WDFWORKITEM Item)
{
    SPI_CONTEXT *c = SpiContext(WdfWorkItemGetParentObject(Item));
    WDFREQUEST request;
    SPI_REQUEST *r;
    SPI_ENGINE *e;
    NTSTATUS status;
    ULONG divider, mask;
    ULONGLONG deadline;
    BOOLEAN done = FALSE, touched = FALSE;
    LARGE_INTEGER timeout;
    WdfSpinLockAcquire(c->Lock); request = c->Request; WdfSpinLockRelease(c->Lock);
    if (!request) return;
    r = SpiRequest(request); e = &r->Engine;
    RtlZeroMemory(e->Tx, e->Frames);
    status = CopyBuffers(r, FALSE);
    if (!NT_SUCCESS(status)) goto finish;
    if (r->Cancelled) { status = STATUS_CANCELLED; goto finish; }
    if (!c->Online) { status = STATUS_DEVICE_POWER_FAILURE; goto finish; }
    status = SpiDivider(c->ClockHz, r->Target.Speed, &divider);
    if (!NT_SUCCESS(status)) goto finish;
    touched = TRUE;
    WdfInterruptAcquireLock(c->Interrupt); Mask(c, 0); WdfInterruptReleaseLock(c->Interrupt);
    Disable(e);
    SpiWrite(e, SPI_CONTROL, c->FrameControl | (r->Target.Mode << 6));
    SpiWrite(e, SPI_COUNT, 0); SpiWrite(e, SPI_DIVIDER, divider);
    SpiWrite(e, SPI_TX_THRESHOLD, c->Depth / 2 - 1); SpiWrite(e, SPI_RX_THRESHOLD, 0);
    SpiWrite(e, SPI_DMA, 0); SpiWrite(e, SPI_RX_DELAY, 0);
    status = Select(&r->Target, TRUE);
    if (!NT_SUCCESS(status)) goto finish;
    SpiWrite(e, SPI_SELECT, 1); // Internal transmit gate; physical CS uses GPIO.
    SpiWrite(e, SPI_ENABLE, 1);
    deadline = KeQueryInterruptTime() + 10000000 + (ULONGLONG)e->Frames * 120000000 / r->Target.Speed;
    timeout.QuadPart = -10000; // Also bounds the final BUSY-bit settling wait.
    for (;;) {
        KeClearEvent(&c->Wake);
        if (r->Cancelled) { status = STATUS_CANCELLED; break; }
        if (!c->Online) { status = STATUS_DEVICE_POWER_FAILURE; break; }
        if (KeQueryInterruptTime() >= deadline) { status = STATUS_IO_TIMEOUT; break; }
        WdfInterruptAcquireLock(c->Interrupt);
        if (c->Online) {
            status = SpiPump(e, &mask, &done);
            Mask(c, NT_SUCCESS(status) && !done ? mask : 0);
        } else status = STATUS_DEVICE_POWER_FAILURE;
        WdfInterruptReleaseLock(c->Interrupt);
        if (!NT_SUCCESS(status) || done) break;
        (void)KeWaitForSingleObject(&c->Wake, Executive, KernelMode, FALSE, &timeout);
    }
finish:
    if (touched) {
        NTSTATUS deselect;
        WdfInterruptAcquireLock(c->Interrupt); Mask(c, 0); WdfInterruptReleaseLock(c->Interrupt);
        Disable(e);
        deselect = Select(&r->Target, FALSE);
        if (NT_SUCCESS(status)) status = deselect;
    }
    if (NT_SUCCESS(status)) status = CopyBuffers(r, TRUE);
    r->Status = status;
    WdfSpinLockAcquire(c->Lock); c->Request = NULL; WdfSpinLockRelease(c->Lock);
    if (WdfRequestUnmarkCancelable(request) != STATUS_CANCELLED) Complete(request);
    else if (InterlockedOr(&r->Completion, 2) & 1) Complete(request);
}

_Use_decl_annotations_
VOID SpiCaller(WDFDEVICE Device, WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS p;
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    WDF_REQUEST_PARAMETERS_INIT(&p);
    WdfRequestGetParameters(Request, &p);
    if (p.Type == WdfRequestTypeDeviceControl && p.Parameters.DeviceIoControl.IoControlCode == IOCTL_SPB_FULL_DUPLEX)
        status = SpbRequestCaptureIoOtherTransferList((SPBREQUEST)Request);
    if (NT_SUCCESS(status)) status = WdfDeviceEnqueueRequest(Device, Request);
    if (!NT_SUCCESS(status)) WdfRequestComplete(Request, status);
}
_Use_decl_annotations_
VOID SpiOther(WDFDEVICE Device, SPBTARGET Target, SPBREQUEST Request, size_t OutputLength, size_t InputLength, ULONG Code)
{
    SPB_REQUEST_PARAMETERS p;
    UNREFERENCED_PARAMETER(OutputLength); UNREFERENCED_PARAMETER(InputLength);
    if (Code != IOCTL_SPB_FULL_DUPLEX) { SpbRequestComplete(Request, STATUS_NOT_SUPPORTED); return; }
    SPB_REQUEST_PARAMETERS_INIT(&p); SpbRequestGetParameters(Request, &p);
    Begin(Device, Target, Request, p.SequenceTransferCount, TRUE);
}
