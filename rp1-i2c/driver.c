// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"
#include <wdf.h>
#include <spbcx.h>
#include <reshub.h>
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
    ULONG Uid, ClockHz, TxDepth, RxDepth, Boot[11];
} I2C_CONTEXT;
typedef struct { ULONG Address, Speed; } I2C_TARGET;
typedef struct {
    I2C_CONTEXT *Controller;
    I2C_TARGET Target;
    I2C_ENGINE Engine;
    PMDL Mdls[I2C_MAX_TRANSFERS];
    ULONG Total;
    volatile LONG Cancelled, Completion;
    NTSTATUS Status;
} I2C_REQUEST;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(I2C_CONTEXT, I2cContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(I2C_TARGET, I2cTarget)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(I2C_REQUEST, I2cRequest)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD I2cAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE I2cPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE I2cRelease;
EVT_WDF_DEVICE_D0_ENTRY I2cStart;
EVT_WDF_DEVICE_D0_EXIT I2cStop;
EVT_WDF_INTERRUPT_ISR I2cIsr;
EVT_WDF_INTERRUPT_DPC I2cDpc;
EVT_WDF_INTERRUPT_ENABLE I2cInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE I2cInterruptDisable;
EVT_WDF_WORKITEM I2cWorker;
EVT_WDF_REQUEST_CANCEL I2cCancel;
EVT_SPB_TARGET_CONNECT I2cConnect;
EVT_SPB_CONTROLLER_READ I2cReadRequest;
EVT_SPB_CONTROLLER_WRITE I2cWriteRequest;
EVT_SPB_CONTROLLER_SEQUENCE I2cSequence;
static const ULONG SavedOffsets[] = { I2C_CON, I2C_TAR, I2C_SS_HIGH, I2C_SS_LOW,
    I2C_FS_HIGH, I2C_FS_LOW, I2C_RX_TL, I2C_TX_TL, I2C_SDA_HOLD, I2C_MASK, I2C_DMA };

static I2C_ENGINE Registers(I2C_CONTEXT *c)
{
    I2C_ENGINE e = {0}; e.Registers = c->Registers; return e;
}
static VOID Mask(I2C_CONTEXT *c, ULONG mask)
{
    WRITE_REGISTER_ULONG((PULONG)(c->Registers + I2C_MASK), mask);
    (void)READ_REGISTER_ULONG((PULONG)(c->Registers + I2C_MASK));
}
static BOOLEAN Disable(I2C_ENGINE *e)
{
    ULONG i;
    LARGE_INTEGER pause;
    pause.QuadPart = -10000; // 1 ms; the caller is always at PASSIVE_LEVEL.
    if (I2cRead(e, I2C_ENABLE_STATUS) & 1) {
        I2cWrite(e, I2C_ENABLE, 3); // Abort an incomplete transfer before disabling.
        for (i = 0; i < 20 && (I2cRead(e, I2C_ENABLE) & 2); ++i)
            (void)KeDelayExecutionThread(KernelMode, FALSE, &pause);
    }
    I2cWrite(e, I2C_ENABLE, 0);
    for (i = 0; i < 20; ++i) {
        if (!(I2cRead(e, I2C_ENABLE_STATUS) & 1)) return TRUE;
        (void)KeDelayExecutionThread(KernelMode, FALSE, &pause);
    }
    return FALSE;
}
_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT Object, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, I2cAdd);
    return WdfDriverCreate(Object, Path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS I2cAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_INTERRUPT_CONFIG interrupt;
    WDF_WORKITEM_CONFIG work;
    SPB_CONTROLLER_CONFIG spb;
    WDFDEVICE device;
    WDFMEMORY memory;
    I2C_CONTEXT *c;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Driver);
    status = SpbDeviceInitConfig(Init);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = I2cPrepare; pnp.EvtDeviceReleaseHardware = I2cRelease;
    pnp.EvtDeviceD0Entry = I2cStart; pnp.EvtDeviceD0Exit = I2cStop;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, I2C_CONTEXT);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&Init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    c = I2cContext(device); c->Device = device;
    KeInitializeEvent(&c->Wake, NotificationEvent, FALSE);
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = device;
    // One bounded buffer per controller avoids allocation failures part-way
    // through a transaction. SpbCx dispatches this controller sequentially.
    status = WdfMemoryCreate(&a, NonPagedPoolNx, 'I1pR', I2C_MAX_BYTES, &memory, (PVOID *)&c->Buffer);
    if (!NT_SUCCESS(status)) return status;
    status = WdfSpinLockCreate(&a, &c->Lock);
    if (!NT_SUCCESS(status)) return status;
    WDF_INTERRUPT_CONFIG_INIT(&interrupt, I2cIsr, I2cDpc);
    interrupt.AutomaticSerialization = FALSE;
    interrupt.EvtInterruptEnable = I2cInterruptEnable;
    interrupt.EvtInterruptDisable = I2cInterruptDisable;
    status = WdfInterruptCreate(device, &interrupt, WDF_NO_OBJECT_ATTRIBUTES, &c->Interrupt);
    if (!NT_SUCCESS(status)) return status;
    WDF_WORKITEM_CONFIG_INIT(&work, I2cWorker); work.AutomaticSerialization = FALSE;
    status = WdfWorkItemCreate(&work, &a, &c->Worker);
    if (!NT_SUCCESS(status)) return status;
    SPB_CONTROLLER_CONFIG_INIT(&spb);
    spb.EvtSpbTargetConnect = I2cConnect;
    spb.EvtSpbIoRead = I2cReadRequest; spb.EvtSpbIoWrite = I2cWriteRequest;
    spb.EvtSpbIoSequence = I2cSequence;
    status = SpbDeviceInitialize(device, &spb);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, I2C_TARGET);
    SpbControllerSetTargetAttributes(device, &a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, I2C_REQUEST);
    SpbControllerSetRequestAttributes(device, &a);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS I2cPrepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    I2C_CONTEXT *c = I2cContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    ULONG i, interrupts = 0, pins = 0, parameters;
    NTSTATUS status;
    I2C_ENGINE e;
    UNREFERENCED_PARAMETER(Raw);
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeMemory) {
            if (memory || r->u.Memory.Length != 0x1000) return STATUS_DEVICE_CONFIGURATION_ERROR;
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
    if (c->Uid > 3) return STATUS_NOT_SUPPORTED;
    c->Registers = MmMapIoSpaceEx(memory->u.Memory.Start, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->Registers) return STATUS_INSUFFICIENT_RESOURCES;
    e = Registers(c);
    if (I2cRead(&e, I2C_TYPE) != 0x44570140 || I2cRead(&e, I2C_VERSION) < 0x3131312a ||
        I2cRead(&e, I2C_ENABLE) || I2cRead(&e, I2C_DMA)) {
        MmUnmapIoSpace(c->Registers, 0x1000); c->Registers = NULL;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    parameters = I2cRead(&e, I2C_PARAMETERS);
    c->TxDepth = ((parameters >> 16) & 255) + 1; c->RxDepth = ((parameters >> 8) & 255) + 1;
    if (c->TxDepth < 2 || c->RxDepth < 2) {
        MmUnmapIoSpace(c->Registers, 0x1000); c->Registers = NULL; return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    for (i = 0; i < RTL_NUMBER_OF(SavedOffsets); ++i) c->Boot[i] = I2cRead(&e, SavedOffsets[i]);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS I2cRelease(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    I2C_CONTEXT *c = I2cContext(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (c->Registers) { MmUnmapIoSpace(c->Registers, 0x1000); c->Registers = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS I2cStart(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    I2C_CONTEXT *c = I2cContext(Device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Previous);
    status = Rp1OpenClock(Device, FALSE, &c->Clock, &c->ClockHz);
    if (!NT_SUCCESS(status)) return status;
    Mask(c, 0);
    status = Rp1OpenInterruptRoute(Device, c->Uid + 7, &c->Route);
    if (!NT_SUCCESS(status)) { Mask(c, c->Boot[9]); WdfObjectDelete(c->Clock); c->Clock = NULL; }
    return status;
}
_Use_decl_annotations_
NTSTATUS I2cStop(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    I2C_CONTEXT *c = I2cContext(Device);
    I2C_ENGINE e = Registers(c);
    ULONG i;
    UNREFERENCED_PARAMETER(Target);
    InterlockedExchange(&c->Online, 0); KeSetEvent(&c->Wake, IO_NO_INCREMENT, FALSE);
    WdfWorkItemFlush(c->Worker);
    Mask(c, 0);
    if (Disable(&e))
        for (i = 0; i < RTL_NUMBER_OF(SavedOffsets); ++i)
            if (SavedOffsets[i] != I2C_MASK) I2cWrite(&e, SavedOffsets[i], c->Boot[i]);
    if (c->Route) { WdfObjectDelete(c->Route); c->Route = NULL; }
    Mask(c, c->Boot[9]);
    if (c->Clock) { WdfObjectDelete(c->Clock); c->Clock = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS I2cInterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Interrupt);
    InterlockedExchange(&I2cContext(Device)->Online, 1);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS I2cInterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    I2C_CONTEXT *c = I2cContext(Device);
    InterlockedExchange(&c->Online, 0); Mask(c, 0);
    (void)WdfInterruptQueueDpcForIsr(Interrupt);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
BOOLEAN I2cIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    I2C_CONTEXT *c = I2cContext(WdfInterruptGetDevice(Interrupt));
    ULONG pending;
    UNREFERENCED_PARAMETER(MessageId);
    if (!c->Online) return FALSE;
    pending = READ_REGISTER_ULONG((PULONG)(c->Registers + I2C_INTR));
    if (!pending) return FALSE;
    Mask(c, 0);
    (void)WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}
_Use_decl_annotations_
VOID I2cDpc(WDFINTERRUPT Interrupt, WDFOBJECT Device)
{
    UNREFERENCED_PARAMETER(Interrupt);
    KeSetEvent(&I2cContext(Device)->Wake, IO_NO_INCREMENT, FALSE);
}

#include <pshpack1.h>
typedef struct {
    PNP_SERIAL_BUS_DESCRIPTOR Bus;
    ULONG Speed;
    USHORT Address;
} I2C_DESCRIPTOR;
#include <poppack.h>
_Use_decl_annotations_
NTSTATUS I2cConnect(WDFDEVICE Controller, SPBTARGET Target)
{
    SPB_CONNECTION_PARAMETERS p;
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER properties;
    I2C_DESCRIPTOR *d;
    I2C_TARGET *t = I2cTarget(Target);
    UNREFERENCED_PARAMETER(Controller);
    SPB_CONNECTION_PARAMETERS_INIT(&p);
    SpbTargetGetConnectionParameters(Target, &p);
    properties = p.ConnectionParameters;
    if (!properties || properties->PropertiesLength < sizeof(*d)) return STATUS_INVALID_PARAMETER;
    d = (I2C_DESCRIPTOR *)properties->ConnectionProperties;
    if (d->Bus.SerialBusType != 1 || (d->Bus.GeneralFlags & 1) ||
        (d->Bus.TypeSpecificFlags & 1) || d->Bus.TypeDataLength < 6 ||
        d->Address < 8 || d->Address > 0x77 ||
        (d->Speed != 100000 && d->Speed != 400000)) return STATUS_NOT_SUPPORTED;
    t->Address = d->Address; t->Speed = d->Speed;
    return STATUS_SUCCESS;
}

// One reference keeps the context alive until the last cancellation participant
// has handed off completion. Bit 0 means cancel has exited; bit 1 means the
// worker has stopped touching hardware and published its result.
static VOID Complete(WDFREQUEST Request)
{
    I2C_REQUEST *r = I2cRequest(Request);
    NTSTATUS status = r->Cancelled ? STATUS_CANCELLED : r->Status;
    WdfRequestSetInformation(Request, NT_SUCCESS(status) ? r->Total : 0);
    SpbRequestComplete((SPBREQUEST)Request, status);
    WdfObjectDereference(Request);
}
_Use_decl_annotations_
VOID I2cCancel(WDFREQUEST Request)
{
    I2C_REQUEST *r = I2cRequest(Request);
    InterlockedExchange(&r->Cancelled, 1);
    KeSetEvent(&r->Controller->Wake, IO_NO_INCREMENT, FALSE);
    if (InterlockedOr(&r->Completion, 1) & 2) Complete(Request);
}
static VOID Begin(WDFDEVICE Device, SPBTARGET Target, SPBREQUEST Request, ULONG Count)
{
    I2C_CONTEXT *c = I2cContext(Device);
    I2C_REQUEST *r = I2cRequest(Request);
    SPB_REQUEST_PARAMETERS p;
    ULONG i;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    SPB_REQUEST_PARAMETERS_INIT(&p);
    SpbRequestGetParameters(Request, &p);
    // A transfer sequence must arrive as one SPB request. Explicit controller
    // locks spanning multiple requests are not supported.
    if (p.Position != SpbRequestSequencePositionSingle || !Count || Count > I2C_MAX_TRANSFERS) goto fail;
    RtlZeroMemory(r, sizeof(*r));
    r->Controller = c; r->Target = *I2cTarget(Target);
    r->Engine.Registers = c->Registers;
    r->Engine.Buffer = c->Buffer;
    r->Engine.TxDepth = c->TxDepth; r->Engine.RxDepth = c->RxDepth;
    r->Engine.TransferCount = Count;
    for (i = 0; i < Count; ++i) {
        SPB_TRANSFER_DESCRIPTOR d;
        I2C_TRANSFER *t = &r->Engine.Transfers[i];
        SPB_TRANSFER_DESCRIPTOR_INIT(&d);
        SpbRequestGetTransferParameters(Request, i, &d, &r->Mdls[i]);
        if (!d.TransferLength || d.TransferLength > I2C_MAX_BYTES - r->Total ||
            !r->Mdls[i] || d.DelayInUs ||
            (d.Direction != SpbTransferDirectionFromDevice && d.Direction != SpbTransferDirectionToDevice)) goto fail;
        t->Read = d.Direction == SpbTransferDirectionFromDevice;
        t->Offset = r->Total; t->Length = (ULONG)d.TransferLength;
        r->Total += t->Length;
    }
    WdfSpinLockAcquire(c->Lock);
    if (c->Request || !c->Online) {
        WdfSpinLockRelease(c->Lock); status = STATUS_DEVICE_NOT_READY; goto fail;
    }
    c->Request = (WDFREQUEST)Request;
    WdfObjectReference(Request);
    WdfSpinLockRelease(c->Lock);
    status = WdfRequestMarkCancelableEx((WDFREQUEST)Request, I2cCancel);
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
VOID I2cReadRequest(WDFDEVICE Device, SPBTARGET Target, SPBREQUEST Request, size_t Length)
{ UNREFERENCED_PARAMETER(Length); Begin(Device, Target, Request, 1); }
_Use_decl_annotations_
VOID I2cWriteRequest(WDFDEVICE Device, SPBTARGET Target, SPBREQUEST Request, size_t Length)
{ UNREFERENCED_PARAMETER(Length); Begin(Device, Target, Request, 1); }
_Use_decl_annotations_
VOID I2cSequence(WDFDEVICE Device, SPBTARGET Target, SPBREQUEST Request, ULONG Count)
{ Begin(Device, Target, Request, Count); }

static NTSTATUS CopyBuffers(I2C_REQUEST *r, BOOLEAN Read)
{
    ULONG i;
    for (i = 0; i < r->Engine.TransferCount; ++i) {
        I2C_TRANSFER *t = &r->Engine.Transfers[i];
        PMDL mdl;
        ULONG copied = 0;
        if (t->Read != Read) continue;
        for (mdl = r->Mdls[i]; mdl && copied < t->Length; mdl = mdl->Next) {
            ULONG bytes = MmGetMdlByteCount(mdl);
            PUCHAR mapped = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
            if (!mapped) return STATUS_INSUFFICIENT_RESOURCES;
            if (bytes > t->Length - copied) bytes = t->Length - copied;
            if (Read) RtlCopyMemory(mapped, r->Engine.Buffer + t->Offset + copied, bytes);
            else RtlCopyMemory(r->Engine.Buffer + t->Offset + copied, mapped, bytes);
            copied += bytes;
        }
        if (copied != t->Length) return STATUS_INVALID_BUFFER_SIZE;
    }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
VOID I2cWorker(WDFWORKITEM Item)
{
    I2C_CONTEXT *c = I2cContext(WdfWorkItemGetParentObject(Item));
    WDFREQUEST request;
    I2C_REQUEST *r;
    I2C_ENGINE *e;
    NTSTATUS status;
    ULONG high, low, hold, mask;
    ULONGLONG deadline;
    BOOLEAN done = FALSE, touched = FALSE;
    LARGE_INTEGER timeout;
    WdfSpinLockAcquire(c->Lock); request = c->Request; WdfSpinLockRelease(c->Lock);
    if (!request) return;
    r = I2cRequest(request); e = &r->Engine;
    status = CopyBuffers(r, FALSE);
    if (!NT_SUCCESS(status)) goto finish;
    if (r->Cancelled) { status = STATUS_CANCELLED; goto finish; }
    if (!c->Online) { status = STATUS_DEVICE_POWER_FAILURE; goto finish; }
    status = I2cTiming(c->ClockHz, r->Target.Speed, &high, &low, &hold);
    if (!NT_SUCCESS(status)) goto finish;
    touched = TRUE;
    WdfInterruptAcquireLock(c->Interrupt); Mask(c, 0); WdfInterruptReleaseLock(c->Interrupt);
    if (!Disable(e)) { status = STATUS_IO_TIMEOUT; goto finish; }
    I2cWrite(e, I2C_CON, 0x261 | (r->Target.Speed == 100000 ? 2 : 4));
    I2cWrite(e, I2C_TAR, r->Target.Address);
    I2cWrite(e, r->Target.Speed == 100000 ? I2C_SS_HIGH : I2C_FS_HIGH, high);
    I2cWrite(e, r->Target.Speed == 100000 ? I2C_SS_LOW : I2C_FS_LOW, low);
    I2cWrite(e, I2C_SDA_HOLD, hold);
    I2cWrite(e, I2C_TX_TL, c->TxDepth / 2 - 1); I2cWrite(e, I2C_RX_TL, 0);
    I2cWrite(e, I2C_DMA, 0);
    (void)I2cRead(e, I2C_CLEAR);
    I2cWrite(e, I2C_ENABLE, 1);
    deadline = KeQueryInterruptTime() + 10000000 + (ULONGLONG)r->Total * 120000000 / r->Target.Speed;
    timeout.QuadPart = -500000; // Wake periodically even if an interrupt is lost.
    for (;;) {
        KeClearEvent(&c->Wake);
        if (r->Cancelled) { status = STATUS_CANCELLED; break; }
        if (!c->Online) { status = STATUS_DEVICE_POWER_FAILURE; break; }
        if (KeQueryInterruptTime() >= deadline) { status = STATUS_IO_TIMEOUT; break; }
        WdfInterruptAcquireLock(c->Interrupt);
        if (c->Online) {
            status = I2cPump(e, &mask, &done);
            Mask(c, NT_SUCCESS(status) && !done ? mask : 0);
        } else status = STATUS_DEVICE_POWER_FAILURE;
        WdfInterruptReleaseLock(c->Interrupt);
        if (!NT_SUCCESS(status) || done) break;
        (void)KeWaitForSingleObject(&c->Wake, Executive, KernelMode, FALSE, &timeout);
    }
finish:
    if (touched) {
        WdfInterruptAcquireLock(c->Interrupt); Mask(c, 0); WdfInterruptReleaseLock(c->Interrupt);
        if (!Disable(e) && NT_SUCCESS(status)) status = STATUS_IO_TIMEOUT;
        (void)I2cRead(e, I2C_CLEAR);
    }
    if (NT_SUCCESS(status)) status = CopyBuffers(r, TRUE);
    r->Status = status;
    WdfSpinLockAcquire(c->Lock); c->Request = NULL; WdfSpinLockRelease(c->Lock);
    if (WdfRequestUnmarkCancelable(request) != STATUS_CANCELLED) Complete(request);
    else if (InterlockedOr(&r->Completion, 2) & 1) Complete(request);
}
