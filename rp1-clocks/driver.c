// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>
#include "rates.h"
#include "rp1-clock.h"
typedef struct {
    PUCHAR Registers;
    WDFWAITLOCK Lock;
    ULONG Users, SavedControl, SavedDivider;
    BOOLEAN Online, Changed;
} CLOCK_CONTEXT;
typedef struct { BOOLEAN Uart; } CLOCK_FILE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CLOCK_CONTEXT, ClockContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CLOCK_FILE, ClockFile)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD ClockAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE ClockPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE ClockRelease;
EVT_WDF_DEVICE_D0_ENTRY ClockStart;
EVT_WDF_DEVICE_D0_EXIT ClockStop;
EVT_WDF_DEVICE_QUERY_REMOVE ClockQueryRemove;
EVT_WDF_DEVICE_QUERY_STOP ClockQueryStop;
EVT_WDF_FILE_CLEANUP ClockCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL ClockIo;
static ULONG Read(CLOCK_CONTEXT *c, ULONG offset)
{ return READ_REGISTER_ULONG((PULONG)(c->Registers + offset)); }
static VOID Write(CLOCK_CONTEXT *c, ULONG offset, ULONG value)
{
    WRITE_REGISTER_ULONG((PULONG)(c->Registers + offset), value);
    (void)Read(c, offset);
}
static VOID EnableUart(CLOCK_CONTEXT *c)
{
    // Called only for a clock that was disabled when the first lease opened.
    // XOSC / 1 gives 50 MHz without changing any shared PLL.
    Write(c, RP1_CLK_UART, 2u << 5);
    Write(c, RP1_CLK_UART + 4, 1);
    Write(c, RP1_CLK_UART, (2u << 5) | RP1_CLK_ENABLE);
}
static VOID Restore(CLOCK_CONTEXT *c)
{
    if (!c->Changed) return;
    Write(c, RP1_CLK_UART, Read(c, RP1_CLK_UART) & ~RP1_CLK_ENABLE);
    Write(c, RP1_CLK_UART + 4, c->SavedDivider);
    Write(c, RP1_CLK_UART, c->SavedControl);
}
_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT Object, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, ClockAdd);
    return WdfDriverCreate(Object, Path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS ClockAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG files;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_IO_QUEUE_CONFIG queue;
    WDFDEVICE device;
    NTSTATUS status;
    UNICODE_STRING name = RTL_CONSTANT_STRING(RP1_CLOCK_NAME);
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\Pi5Rp1Clock");
    UNREFERENCED_PARAMETER(Driver);
    status = WdfDeviceInitAssignName(Init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(Init, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = ClockPrepare; pnp.EvtDeviceReleaseHardware = ClockRelease;
    pnp.EvtDeviceD0Entry = ClockStart; pnp.EvtDeviceD0Exit = ClockStop;
    pnp.EvtDeviceQueryRemove = ClockQueryRemove; pnp.EvtDeviceQueryStop = ClockQueryStop;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&files, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, ClockCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, CLOCK_FILE);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    WdfDeviceInitSetFileObjectConfig(Init, &files, &a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, CLOCK_CONTEXT);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&Init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = device;
    status = WdfWaitLockCreate(&a, &ClockContext(device)->Lock);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = ClockIo;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS ClockPrepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    ULONG i;
    UNREFERENCED_PARAMETER(Raw);
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeMemory) {
            if (memory || r->u.Memory.Length != 0x10038) return STATUS_DEVICE_CONFIGURATION_ERROR;
            memory = r;
        }
    }
    if (!memory) return STATUS_DEVICE_CONFIGURATION_ERROR;
    ClockContext(Device)->Registers = MmMapIoSpaceEx(memory->u.Memory.Start, 0x10038, PAGE_READWRITE | PAGE_NOCACHE);
    return ClockContext(Device)->Registers ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}
_Use_decl_annotations_
NTSTATUS ClockRelease(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    CLOCK_CONTEXT *c = ClockContext(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (c->Registers) { MmUnmapIoSpace(c->Registers, 0x10038); c->Registers = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS ClockStart(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    CLOCK_CONTEXT *c = ClockContext(Device);
    UNREFERENCED_PARAMETER(Previous);
    WdfWaitLockAcquire(c->Lock, NULL);
    if (c->Users && c->Changed) EnableUart(c);
    c->Online = TRUE;
    WdfWaitLockRelease(c->Lock);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS ClockStop(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    CLOCK_CONTEXT *c = ClockContext(Device);
    UNREFERENCED_PARAMETER(Target);
    WdfWaitLockAcquire(c->Lock, NULL);
    Restore(c); c->Online = FALSE;
    WdfWaitLockRelease(c->Lock);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS ClockQueryRemove(WDFDEVICE Device)
{
    CLOCK_CONTEXT *c = ClockContext(Device);
    NTSTATUS status;
    WdfWaitLockAcquire(c->Lock, NULL);
    status = c->Users ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    WdfWaitLockRelease(c->Lock);
    return status;
}
_Use_decl_annotations_
NTSTATUS ClockQueryStop(WDFDEVICE Device) { return ClockQueryRemove(Device); }
_Use_decl_annotations_
VOID ClockCleanup(WDFFILEOBJECT File)
{
    CLOCK_CONTEXT *c = ClockContext(WdfFileObjectGetDevice(File));
    WdfWaitLockAcquire(c->Lock, NULL);
    if (ClockFile(File)->Uart) {
        ClockFile(File)->Uart = FALSE;
        if (--c->Users == 0) {
            if (c->Online) Restore(c);
            c->Changed = FALSE;
        }
    }
    WdfWaitLockRelease(c->Lock);
}
_Use_decl_annotations_
VOID ClockIo(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    CLOCK_CONTEXT *c = ClockContext(WdfIoQueueGetDevice(Queue));
    RP1_CLOCK_STATUS *r;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(OutLength); UNREFERENCED_PARAMETER(InLength);
    if ((Code != IOCTL_RP1_CLOCK_QUERY && Code != IOCTL_RP1_CLOCK_UART) ||
        !WdfRequestGetFileObject(Request) ||
        (Code == IOCTL_RP1_CLOCK_UART && WdfRequestGetRequestorMode(Request) != KernelMode)) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST); return;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*r), (PVOID *)&r, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    WdfWaitLockAcquire(c->Lock, NULL);
    if (!c->Online) status = STATUS_DEVICE_NOT_READY;
    else {
        CLOCK_FILE *f = ClockFile(WdfRequestGetFileObject(Request));
        if (Code == IOCTL_RP1_CLOCK_UART && !f->Uart) {
            if (!c->Users) {
                c->SavedControl = Read(c, RP1_CLK_UART);
                c->SavedDivider = Read(c, RP1_CLK_UART + 4);
                c->Changed = !(c->SavedControl & RP1_CLK_ENABLE);
                if (c->Changed) EnableUart(c);
            }
            if (!Rp1ClockRate(c->Registers, TRUE)) {
                if (!c->Users) { Restore(c); c->Changed = FALSE; }
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
            } else { ++c->Users; f->Uart = TRUE; }
        }
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(r, sizeof(*r));
            r->Version = 1; r->Online = c->Online;
            r->SystemHz = Rp1ClockRate(c->Registers, FALSE); r->UartHz = Rp1ClockRate(c->Registers, TRUE);
            r->UartUsers = c->Users;
            r->SystemControl = Read(c, RP1_CLK_SYS); r->SystemDivider = Read(c, RP1_CLK_SYS + 4);
            r->UartControl = Read(c, RP1_CLK_UART); r->UartDivider = Read(c, RP1_CLK_UART + 4);
        }
    }
    WdfWaitLockRelease(c->Lock);
    WdfRequestCompleteWithInformation(Request, status, NT_SUCCESS(status) ? sizeof(*r) : 0);
}
