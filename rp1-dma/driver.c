// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>
#define RP1_SERVICE_CLIENT
#include "rp1-service.h"
#include "rp1-clock.h"
#include "rp1-dma.h"
#include "hardware.h"
typedef struct {
    PUCHAR Registers, Buffer;
    PDMA_ADAPTER Adapter;
    PHYSICAL_ADDRESS Address;
    WDFIOTARGET Clock, Route;
    WDFINTERRUPT Interrupt;
    KEVENT Wake;
    volatile LONG Online, Pending, Interrupts;
    ULONG ClockHz, Id, Version, Transfers, Errors, LastStatus, LastInterrupt;
    ULONG Saved[7];
    BOOLEAN Owned, Unsafe;
} DMA_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DMA_CONTEXT, DmaContext)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD DmaAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE DmaPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE DmaRelease;
EVT_WDF_DEVICE_D0_ENTRY DmaStart;
EVT_WDF_DEVICE_D0_EXIT DmaStop;
EVT_WDF_INTERRUPT_ISR DmaIsr;
EVT_WDF_INTERRUPT_DPC DmaDpc;
EVT_WDF_INTERRUPT_ENABLE DmaInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE DmaInterruptDisable;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL DmaIo;
static const ULONG SavedOffsets[] = { DMA_CONFIG, DMA_COMMON_ENABLE, DMA_COMMON_SIGNAL,
    DMA_CH_CONFIG, DMA_CH_CONFIG + 4, DMA_CH_INT_ENABLE, DMA_CH_INT_SIGNAL };
static VOID Snapshot(WDFDEVICE Device, ULONG Stage, NTSTATUS Status)
{
    DMA_CONTEXT *c = DmaContext(Device);
    ULONG data[] = {1, Stage, (ULONG)Status, c->Id, c->Version, c->ClockHz,
        (ULONG)c->Interrupts, c->Transfers, c->Errors, c->LastInterrupt, c->Unsafe};
    WDFKEY key;
    DECLARE_CONST_UNICODE_STRING(name, L"DmaDiagnostics");
    c->LastStatus = (ULONG)Status;
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignValue(key, &name, REG_BINARY, sizeof(data), data);
        WdfRegistryClose(key);
    }
#if DBG
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "Pi5Dma: stage %lu status %08lx version %08lx clock %lu irq %ld transfers %lu\n",
        Stage, Status, c->Version, c->ClockHz, c->Interrupts, c->Transfers);
#endif
}
static NTSTATUS OpenClock(WDFDEVICE Device)
{
    DMA_CONTEXT *c = DmaContext(Device);
    WDF_OBJECT_ATTRIBUTES a;
    WDF_IO_TARGET_OPEN_PARAMS open;
    WDF_MEMORY_DESCRIPTOR output;
    RP1_DMA_CLOCK_STATUS result = {0};
    ULONG_PTR bytes = 0;
    UNICODE_STRING name = RTL_CONSTANT_STRING(RP1_CLOCK_NAME);
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = Device;
    status = WdfIoTargetCreate(Device, &a, &c->Clock);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_READ | GENERIC_WRITE);
    open.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    status = WdfIoTargetOpen(c->Clock, &open);
    if (NT_SUCCESS(status)) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &result, sizeof(result));
        status = WdfIoTargetSendIoctlSynchronously(c->Clock, NULL,
            IOCTL_RP1_CLOCK_DMA, NULL, &output, NULL, &bytes);
        if (NT_SUCCESS(status) && (bytes != sizeof(result) || result.Version != 1 ||
            !result.Online || !result.SystemHz || !result.DmaHz || result.DmaHz > 100000000))
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
        c->ClockHz = result.DmaHz;
    }
    if (!NT_SUCCESS(status)) { WdfObjectDelete(c->Clock); c->Clock = NULL; }
    return status;
}
_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT Object, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, DmaAdd);
    return WdfDriverCreate(Object, Path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS DmaAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_INTERRUPT_CONFIG interrupt;
    WDFDEVICE device;
    NTSTATUS status;
    UNICODE_STRING name = RTL_CONSTANT_STRING(RP1_DMA_NAME);
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\Pi5Dma");
    UNREFERENCED_PARAMETER(Driver);
    status = WdfDeviceInitAssignName(Init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(Init, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = DmaPrepare; pnp.EvtDeviceReleaseHardware = DmaRelease;
    pnp.EvtDeviceD0Entry = DmaStart; pnp.EvtDeviceD0Exit = DmaStop;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, DMA_CONTEXT);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&Init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    KeInitializeEvent(&DmaContext(device)->Wake, NotificationEvent, FALSE);
    WDF_INTERRUPT_CONFIG_INIT(&interrupt, DmaIsr, DmaDpc);
    interrupt.AutomaticSerialization = FALSE;
    interrupt.EvtInterruptEnable = DmaInterruptEnable; interrupt.EvtInterruptDisable = DmaInterruptDisable;
    status = WdfInterruptCreate(device, &interrupt, WDF_NO_OBJECT_ATTRIBUTES, &DmaContext(device)->Interrupt);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = DmaIo;
    // Each request runs synchronously for at most its fixed test/transfer bound.
    // No marked-cancelable request or DMA mapping outlives this callback.
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS DmaPrepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    DMA_CONTEXT *c = DmaContext(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    ULONG i, interrupts = 0, maps = 0;
    DEVICE_DESCRIPTION description = {0};
    UNREFERENCED_PARAMETER(Raw);
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeMemory) {
            if (memory || r->u.Memory.Length != DMA_REG_SIZE) return STATUS_DEVICE_CONFIGURATION_ERROR;
            memory = r;
        } else if (r->Type == CmResourceTypeInterrupt) {
            if ((r->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED)) ||
                r->ShareDisposition != CmResourceShareShared) return STATUS_DEVICE_CONFIGURATION_ERROR;
            ++interrupts;
        }
    }
    if (!memory || interrupts != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->Registers = MmMapIoSpaceEx(memory->u.Memory.Start, DMA_REG_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->Registers) return STATUS_INSUFFICIENT_RESOURCES;
    description.Version = DEVICE_DESCRIPTION_VERSION3;
    description.Master = TRUE; description.ScatterGather = TRUE;
    description.InterfaceType = Internal; description.DmaAddressWidth = 32;
    description.MaximumLength = DMA_ALLOCATION;
    c->Adapter = IoGetDmaAdapter(WdfDeviceWdmGetPhysicalDevice(Device), &description, &maps);
    if (!c->Adapter) return STATUS_INSUFFICIENT_RESOURCES;
    // HAL resolves the RP1 ACPI DMA aperture and _CCA=0. Never substitute CPU PA.
    c->Buffer = c->Adapter->DmaOperations->AllocateCommonBuffer(c->Adapter, DMA_ALLOCATION, &c->Address, FALSE);
    if (!c->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    if (((ULONGLONG)c->Address.QuadPart & 63) || (ULONGLONG)c->Address.QuadPart > 0x100000000ULL - DMA_ALLOCATION)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlZeroMemory(c->Buffer, DMA_ALLOCATION);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS DmaRelease(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    DMA_CONTEXT *c = DmaContext(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (!c->Unsafe) {
        if (c->Buffer) c->Adapter->DmaOperations->FreeCommonBuffer(c->Adapter, DMA_ALLOCATION, c->Address, c->Buffer, FALSE);
        if (c->Adapter) c->Adapter->DmaOperations->PutDmaAdapter(c->Adapter);
    }
    // A controller which failed both halt and reset must never DMA into freed
    // memory. Retain this small HAL allocation until reboot on that fatal path.
    c->Buffer = NULL; c->Adapter = NULL;
    if (c->Registers) { MmUnmapIoSpace(c->Registers, DMA_REG_SIZE); c->Registers = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS DmaStart(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    DMA_CONTEXT *c = DmaContext(Device);
    NTSTATUS status;
    ULONG i;
    UNREFERENCED_PARAMETER(Previous);
    if (c->Unsafe) return STATUS_DEVICE_HARDWARE_ERROR;
    status = OpenClock(Device);
    if (!NT_SUCCESS(status)) { Snapshot(Device, 10, status); return status; }
    c->Id = DmaRead(c->Registers, DMA_ID); c->Version = DmaRead(c->Registers, DMA_VERSION);
    // RP1 reports 1.03a in COMPVER; the Linux "axi-dma-1.01a" compatible
    // identifies the register interface, not the instantiated IP revision.
    if (c->Version != 0x3130332a || (DmaRead(c->Registers, DMA_CHANNELS) & 0xff)) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR; goto Failed;
    }
    for (i = 0; i < RTL_NUMBER_OF(SavedOffsets); ++i) c->Saved[i] = DmaRead(c->Registers, SavedOffsets[i]);
    c->Owned = TRUE;
    DmaWrite(c->Registers, DMA_CH_INT_SIGNAL, 0);
    DmaWrite(c->Registers, DMA_CH_INT_CLEAR, 0xffffffffu);
    DmaWrite(c->Registers, DMA_COMMON_SIGNAL, 0);
    DmaWrite(c->Registers, DMA_COMMON_CLEAR, 0x1f);
    DmaWrite(c->Registers, DMA_CONFIG, 3);
    status = Rp1OpenInterruptRoute(Device, 40, &c->Route);
    if (!NT_SUCCESS(status)) {
        for (i = 0; i < RTL_NUMBER_OF(SavedOffsets); ++i) DmaWrite(c->Registers, SavedOffsets[i], c->Saved[i]);
        c->Owned = FALSE; goto Failed;
    }
    Snapshot(Device, 20, STATUS_SUCCESS);
    return STATUS_SUCCESS;
Failed:
    Snapshot(Device, 15, status);
    WdfObjectDelete(c->Clock); c->Clock = NULL;
    return status;
}
_Use_decl_annotations_
NTSTATUS DmaStop(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    DMA_CONTEXT *c = DmaContext(Device);
    ULONG i;
    UNREFERENCED_PARAMETER(Target);
    InterlockedExchange(&c->Online, 0);
    if (c->Owned) {
        c->Unsafe = !DmaHalt(c->Registers);
        DmaWrite(c->Registers, DMA_CONFIG, 0);
        if (!c->Unsafe) for (i = 0; i < RTL_NUMBER_OF(SavedOffsets); ++i)
            DmaWrite(c->Registers, SavedOffsets[i], c->Saved[i]);
        c->Owned = FALSE;
    }
    if (c->Route) { WdfObjectDelete(c->Route); c->Route = NULL; }
    if (c->Clock) { WdfObjectDelete(c->Clock); c->Clock = NULL; }
    Snapshot(Device, 90, c->Unsafe ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_SUCCESS);
    return c->Unsafe ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS DmaInterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{ UNREFERENCED_PARAMETER(Interrupt); InterlockedExchange(&DmaContext(Device)->Online, 1); return STATUS_SUCCESS; }
_Use_decl_annotations_
NTSTATUS DmaInterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE Device)
{
    DMA_CONTEXT *c = DmaContext(Device);
    UNREFERENCED_PARAMETER(Interrupt);
    InterlockedExchange(&c->Online, 0); DmaWrite(c->Registers, DMA_CH_INT_SIGNAL, 0);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
BOOLEAN DmaIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    DMA_CONTEXT *c = DmaContext(WdfInterruptGetDevice(Interrupt));
    ULONG bits;
    UNREFERENCED_PARAMETER(MessageId);
    if (!c->Online) return FALSE;
    bits = DmaRead(c->Registers, DMA_CH_INT_STATUS);
    if (!(bits & (DMA_COMPLETE | DMA_ERRORS))) return FALSE;
    DmaWrite(c->Registers, DMA_CH_INT_SIGNAL, 0);
    DmaWrite(c->Registers, DMA_CH_INT_CLEAR, bits);
    InterlockedOr(&c->Pending, (LONG)bits); InterlockedIncrement(&c->Interrupts);
    (void)WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}
_Use_decl_annotations_
VOID DmaDpc(WDFINTERRUPT Interrupt, WDFOBJECT Device)
{ UNREFERENCED_PARAMETER(Interrupt); KeSetEvent(&DmaContext((WDFDEVICE)Device)->Wake, IO_NO_INCREMENT, FALSE); }
static NTSTATUS Copy(DMA_CONTEXT *c, ULONG SourceOffset, ULONG DestinationOffset, ULONG Length)
{
    NTSTATUS status;
    LARGE_INTEGER timeout;
    ULONGLONG deadline;
    if (!c->Online || c->Unsafe) return STATUS_DEVICE_NOT_READY;
    status = DmaDescriptor((RP1_DMA_DESCRIPTOR *)c->Buffer,
        c->Address.QuadPart + SourceOffset, c->Address.QuadPart + DestinationOffset, Length);
    if (!NT_SUCCESS(status)) return status;
    KeMemoryBarrier();
    WdfInterruptAcquireLock(c->Interrupt);
    KeClearEvent(&c->Wake); InterlockedExchange(&c->Pending, 0);
    status = DmaBegin(c->Registers, (ULONGLONG)c->Address.QuadPart);
    WdfInterruptReleaseLock(c->Interrupt);
    if (NT_SUCCESS(status)) {
        deadline = KeQueryInterruptTime() + 5000000ULL;
        for (;;) {
            // An old coalesced DPC may wake us. The IRQ status, protected by
            // the interrupt lock, is authoritative; recheck within one bound.
            WdfInterruptAcquireLock(c->Interrupt);
            KeClearEvent(&c->Wake);
            c->LastInterrupt = (ULONG)c->Pending;
            WdfInterruptReleaseLock(c->Interrupt);
            if (c->LastInterrupt) break;
            timeout.QuadPart = (LONGLONG)(KeQueryInterruptTime() - deadline);
            if (timeout.QuadPart >= 0) { status = STATUS_IO_TIMEOUT; break; }
            status = KeWaitForSingleObject(&c->Wake, Executive, KernelMode, FALSE, &timeout);
            if (status == STATUS_TIMEOUT) { status = STATUS_IO_TIMEOUT; break; }
            if (!NT_SUCCESS(status)) break;
        }
        if (NT_SUCCESS(status) && ((c->LastInterrupt & DMA_ERRORS) || !(c->LastInterrupt & DMA_COMPLETE)))
            status = STATUS_DEVICE_HARDWARE_ERROR;
    }
    WdfInterruptAcquireLock(c->Interrupt);
    DmaWrite(c->Registers, DMA_CH_INT_SIGNAL, 0);
    WdfInterruptReleaseLock(c->Interrupt);
    if (!DmaHalt(c->Registers)) { c->Unsafe = TRUE; status = STATUS_DEVICE_HARDWARE_ERROR; }
    if (!c->Unsafe) DmaWrite(c->Registers, DMA_CONFIG, 3);
    KeMemoryBarrier();
    if (NT_SUCCESS(status)) ++c->Transfers; else ++c->Errors;
    c->LastStatus = (ULONG)status;
    return status;
}
static NTSTATUS SelfTest(DMA_CONTEXT *c, RP1_DMA_TEST_RESULT *r, WDFREQUEST Request)
{
    static const ULONG lengths[] = {1, 2, 3, 15, 16, 31, 64, 255, 256, 4095, 4096, 65536};
    ULONG n, i, alignment, irq = (ULONG)c->Interrupts;
    NTSTATUS status = STATUS_SUCCESS;
    RtlZeroMemory(r, sizeof(*r)); r->Version = 1;
    for (alignment = 0; alignment < 2; ++alignment) for (n = 0; n < RTL_NUMBER_OF(lengths); ++n) {
        ULONG source = DMA_SOURCE_OFFSET + alignment, dest = DMA_DEST_OFFSET + alignment;
        ULONG length = lengths[n];
        if (WdfRequestIsCanceled(Request)) return STATUS_CANCELLED;
        RtlFillMemory(c->Buffer + DMA_SOURCE_OFFSET - 64, DMA_BUFFER_LENGTH + 129, 0xa5);
        RtlFillMemory(c->Buffer + DMA_DEST_OFFSET - 64, DMA_BUFFER_LENGTH + 129, 0x5a);
        for (i = 0; i < length; ++i) c->Buffer[source + i] = (UCHAR)(i * 73u + n * 19u + alignment);
        status = Copy(c, source, dest, length);
        if (!NT_SUCCESS(status)) goto Done;
        for (i = 0; i < length; ++i) {
            UCHAR expected = (UCHAR)(i * 73u + n * 19u + alignment);
            if (c->Buffer[source + i] != expected || c->Buffer[dest + i] != expected) ++r->DataErrors;
        }
        for (i = 1; i <= 64; ++i) if (c->Buffer[source - i] != 0xa5 || c->Buffer[source + length + i - 1] != 0xa5 ||
            c->Buffer[dest - i] != 0x5a || c->Buffer[dest + length + i - 1] != 0x5a) ++r->GuardErrors;
        ++r->Tests; r->Bytes += length;
        if (r->DataErrors || r->GuardErrors) { status = STATUS_DATA_ERROR; goto Done; }
    }
Done:
    r->Interrupts = (ULONG)c->Interrupts - irq;
    return status;
}
_Use_decl_annotations_
VOID DmaIo(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DMA_CONTEXT *c = DmaContext(device);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t bytes = 0;
    UNREFERENCED_PARAMETER(OutLength);
    if (Code == IOCTL_RP1_DMA_QUERY) {
        RP1_DMA_STATUS *r;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*r), (PVOID *)&r, NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(r, sizeof(*r)); r->Version = 1; r->Online = c->Online && !c->Unsafe;
            r->ControllerId = c->Id; r->ComponentVersion = c->Version; r->ClockHz = c->ClockHz;
            r->Interrupts = (ULONG)c->Interrupts; r->Transfers = c->Transfers; r->Errors = c->Errors;
            r->LastStatus = c->LastStatus; r->LastInterrupt = c->LastInterrupt;
            r->ChannelEnable = DmaRead(c->Registers, DMA_CHANNELS); bytes = sizeof(*r);
        }
    } else if (Code == IOCTL_RP1_DMA_SELFTEST && !InLength) {
        RP1_DMA_TEST_RESULT *r;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*r), (PVOID *)&r, NULL);
        if (NT_SUCCESS(status)) { status = SelfTest(c, r, Request); bytes = sizeof(*r); Snapshot(device, 40, status); }
    } else if (Code == IOCTL_RP1_DMA_COPY && WdfRequestGetRequestorMode(Request) == KernelMode) {
        RP1_DMA_COPY *input;
        PUCHAR output;
        ULONG length;
        status = WdfRequestRetrieveInputBuffer(Request, FIELD_OFFSET(RP1_DMA_COPY, Data), (PVOID *)&input, NULL);
        if (NT_SUCCESS(status)) {
            length = input->Length;
            if (input->Version != 1 || !length || length > RP1_DMA_MAX_COPY ||
                InLength != FIELD_OFFSET(RP1_DMA_COPY, Data) + length) status = STATUS_INVALID_PARAMETER;
            else {
                status = WdfRequestRetrieveOutputBuffer(Request, length, (PVOID *)&output, NULL);
                if (NT_SUCCESS(status)) {
                    RtlCopyMemory(c->Buffer + DMA_SOURCE_OFFSET, input->Data, length);
                    status = Copy(c, DMA_SOURCE_OFFSET, DMA_DEST_OFFSET, length);
                    if (NT_SUCCESS(status)) { RtlCopyMemory(output, c->Buffer + DMA_DEST_OFFSET, length); bytes = length; }
                }
            }
        }
    }
    if (c->Unsafe) WdfDeviceSetFailed(device, WdfDeviceFailedNoRestart);
    WdfRequestCompleteWithInformation(Request, status, bytes);
}
