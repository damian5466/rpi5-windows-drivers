// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>
#include "rp1-service.h"

#define CFG(n) (8u + 4u * (n))
#define ROUTE_ENABLE 1u
#define ROUTE_IACK_ENABLE 8u
typedef struct {
    PUCHAR Registers;
    WDFWAITLOCK Lock;
    ULONGLONG Owned;
    BOOLEAN Online;
} RP1_CONTEXT;
typedef struct { ULONGLONG Owned; } RP1_FILE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RP1_CONTEXT, Rp1Context)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(RP1_FILE, Rp1File)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Rp1Add;
EVT_WDF_DEVICE_PREPARE_HARDWARE Rp1Prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE Rp1Release;
EVT_WDF_DEVICE_D0_ENTRY Rp1Start;
EVT_WDF_DEVICE_D0_EXIT Rp1Stop;
EVT_WDF_DEVICE_QUERY_REMOVE Rp1QueryRemove;
EVT_WDF_DEVICE_QUERY_STOP Rp1QueryStop;
EVT_WDF_FILE_CLEANUP Rp1Cleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Rp1Io;

static ULONG ReadConfig(RP1_CONTEXT *c, ULONG source)
{ return READ_REGISTER_ULONG((PULONG)(c->Registers + CFG(source))); }
static VOID Enable(RP1_CONTEXT *c, ULONG source, BOOLEAN enable)
{
    WRITE_REGISTER_ULONG((PULONG)(c->Registers + (enable ? 0x800 : 0xc00) + CFG(source)), ROUTE_ENABLE);
    (void)ReadConfig(c, source); // Drain the posted PCIe write before returning.
}
static BOOLEAN Allowed(ULONG source)
{
    // Header GPIO banks, Ethernet, I2C, SPI and UARTs. USB is never leased.
    return source <= 2 || source == 6 || (source >= 7 && source <= 13) ||
        (source >= 19 && source <= 25 && source != 23) ||
        (source >= 42 && source <= 46) || source == 54 || source == 56;
}
_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, Rp1Add);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS Rp1Add(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_OBJECT_ATTRIBUTES a;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG files;
    WDF_IO_QUEUE_CONFIG queue;
    WDFDEVICE device;
    NTSTATUS status;
    UNICODE_STRING name = RTL_CONSTANT_STRING(RP1_SERVICE_NAME);
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\Pi5Rp1");
    UNREFERENCED_PARAMETER(Driver);
    status = WdfDeviceInitAssignName(Init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(Init, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = Rp1Prepare;
    pnp.EvtDeviceReleaseHardware = Rp1Release;
    pnp.EvtDeviceD0Entry = Rp1Start;
    pnp.EvtDeviceD0Exit = Rp1Stop;
    pnp.EvtDeviceQueryRemove = Rp1QueryRemove;
    pnp.EvtDeviceQueryStop = Rp1QueryStop;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&files, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, Rp1Cleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, RP1_FILE);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    WdfDeviceInitSetFileObjectConfig(Init, &files, &a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, RP1_CONTEXT);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&Init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = device;
    status = WdfWaitLockCreate(&a, &Rp1Context(device)->Lock);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = Rp1Io;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS Rp1Prepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    RP1_CONTEXT *c = Rp1Context(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR reg = NULL;
    ULONG i, memories = 0, interrupts = 0;
    UNREFERENCED_PARAMETER(Raw);
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (r->Type == CmResourceTypeMemory) { if (memories++ == 0) reg = r; }
        if (r->Type == CmResourceTypeInterrupt) {
            if ((r->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) ||
                (r->Flags & CM_RESOURCE_INTERRUPT_LATCHED) || r->ShareDisposition != CmResourceShareShared)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            ++interrupts;
        }
    }
    if (!reg || memories != 4 || interrupts != 1 || reg->u.Memory.Length != 0x1000)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->Registers = MmMapIoSpaceEx(reg->u.Memory.Start, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    return c->Registers ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}
_Use_decl_annotations_
NTSTATUS Rp1Release(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    RP1_CONTEXT *c = Rp1Context(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (c->Registers) { MmUnmapIoSpace(c->Registers, 0x1000); c->Registers = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS Rp1Start(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    RP1_CONTEXT *c = Rp1Context(Device);
    ULONG i;
    UNREFERENCED_PARAMETER(Previous);
    WdfWaitLockAcquire(c->Lock, NULL);
    c->Online = TRUE;
    for (i = 0; i < 61; ++i) if (c->Owned & (1ull << i)) Enable(c, i, TRUE);
    WdfWaitLockRelease(c->Lock);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS Rp1Stop(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    RP1_CONTEXT *c = Rp1Context(Device);
    ULONG i;
    UNREFERENCED_PARAMETER(Target);
    WdfWaitLockAcquire(c->Lock, NULL);
    for (i = 0; i < 61; ++i) if (c->Owned & (1ull << i)) Enable(c, i, FALSE);
    c->Online = FALSE;
    WdfWaitLockRelease(c->Lock);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS Rp1QueryRemove(WDFDEVICE Device)
{
    RP1_CONTEXT *c = Rp1Context(Device);
    NTSTATUS status;
    WdfWaitLockAcquire(c->Lock, NULL);
    status = c->Owned ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    WdfWaitLockRelease(c->Lock);
    return status;
}
_Use_decl_annotations_
NTSTATUS Rp1QueryStop(WDFDEVICE Device) { return Rp1QueryRemove(Device); }
_Use_decl_annotations_
VOID Rp1Cleanup(WDFFILEOBJECT File)
{
    RP1_CONTEXT *c = Rp1Context(WdfFileObjectGetDevice(File));
    RP1_FILE *f = Rp1File(File);
    ULONG i;
    WdfWaitLockAcquire(c->Lock, NULL);
    for (i = 0; i < 61; ++i) if ((f->Owned & (1ull << i)) && c->Online) Enable(c, i, FALSE);
    c->Owned &= ~f->Owned;
    f->Owned = 0;
    WdfWaitLockRelease(c->Lock);
}
_Use_decl_annotations_
VOID Rp1Io(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength, size_t InputLength, ULONG Code)
{
    RP1_CONTEXT *c = Rp1Context(WdfIoQueueGetDevice(Queue));
    RP1_FILE *f;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t information = 0;
    UNREFERENCED_PARAMETER(OutputLength);
    UNREFERENCED_PARAMETER(InputLength);
    if (!WdfRequestGetFileObject(Request)) { WdfRequestComplete(Request, STATUS_INVALID_HANDLE); return; }
    f = Rp1File(WdfRequestGetFileObject(Request));
    WdfWaitLockAcquire(c->Lock, NULL);
    if (!c->Online) status = STATUS_DEVICE_NOT_READY;
    else if (Code == IOCTL_RP1_ACQUIRE_IRQ && WdfRequestGetRequestorMode(Request) == KernelMode) {
        RP1_IRQ_REQUEST *r;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*r), (PVOID *)&r, NULL);
        if (NT_SUCCESS(status)) {
            if (r->Version != RP1_SERVICE_VERSION || !Allowed(r->Source)) status = STATUS_INVALID_PARAMETER;
            else if (c->Owned & (1ull << r->Source)) status = STATUS_SHARING_VIOLATION;
            else if (ReadConfig(c, r->Source) & (ROUTE_ENABLE | ROUTE_IACK_ENABLE)) status = STATUS_DEVICE_BUSY;
            else {
                f->Owned |= 1ull << r->Source;
                c->Owned |= 1ull << r->Source;
                Enable(c, r->Source, TRUE);
            }
        }
    } else if (Code == IOCTL_RP1_QUERY) {
        RP1_SERVICE_STATUS *r;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*r), (PVOID *)&r, NULL);
        if (NT_SUCCESS(status)) {
            ULONG i;
            RtlZeroMemory(r, sizeof(*r));
            r->Version = RP1_SERVICE_VERSION; r->Online = c->Online; r->Owned = c->Owned;
            for (i = 0; i < 61; ++i) r->Config[i] = ReadConfig(c, i);
            information = sizeof(*r);
        }
    }
    WdfWaitLockRelease(c->Lock);
    WdfRequestCompleteWithInformation(Request, status, information);
}
