// SPDX-License-Identifier: BSD-2-Clause-Patent
#define _NO_CRT_STDIO_INLINE
#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>
#include <ntstrsafe.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <gpio.h>
#include <acpiioct.h>
#include "board.h"

typedef struct {
    WDFDEVICE Device;
    WDFWAITLOCK Lock;
    LARGE_INTEGER Connections[PI5_BOARD_PINS];
    WDFIOTARGET Pins[PI5_BOARD_PINS];
    ULONG Configured, Leased, Levels;
    BOOLEAN Online;
} BOARD_CONTEXT;
typedef struct { ULONG Leased; } BOARD_FILE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BOARD_CONTEXT, BoardContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BOARD_FILE, BoardFile)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD BoardAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE BoardPrepare;
EVT_WDF_DEVICE_D0_ENTRY BoardStart;
EVT_WDF_DEVICE_D0_EXIT BoardStop;
EVT_WDF_DEVICE_QUERY_REMOVE BoardQueryRemove;
EVT_WDF_DEVICE_QUERY_STOP BoardQueryStop;
EVT_WDF_FILE_CLEANUP BoardCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL BoardIo;

static NTSTATUS Transfer(WDFIOTARGET Target, BOOLEAN Write, UCHAR *Value)
{
    WDF_MEMORY_DESCRIPTOR buffer;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&buffer, Value, sizeof(*Value));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(3));
    return WdfIoTargetSendIoctlSynchronously(Target, NULL,
        Write ? IOCTL_GPIO_WRITE_PINS : IOCTL_GPIO_READ_PINS,
        Write ? &buffer : NULL, &buffer, &options, NULL);
}
static VOID ClosePin(BOARD_CONTEXT *c, ULONG Pin)
{
    if (c->Pins[Pin]) { WdfObjectDelete(c->Pins[Pin]); c->Pins[Pin] = NULL; }
    c->Configured &= ~(1u << Pin);
}
static NTSTATUS SetPin(BOARD_CONTEXT *c, ULONG Pin, UCHAR Value)
{
    WCHAR path[RESOURCE_HUB_PATH_SIZE / sizeof(WCHAR)];
    UNICODE_STRING name;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_IO_TARGET_OPEN_PARAMS open;
    NTSTATUS status;
    BOOLEAN created = FALSE;
    if (!c->Pins[Pin]) {
        RtlInitEmptyUnicodeString(&name, path, sizeof(path));
        status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&name, c->Connections[Pin].LowPart, c->Connections[Pin].HighPart);
        if (!NT_SUCCESS(status)) return status;
        WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = c->Device;
        status = WdfIoTargetCreate(c->Device, &a, &c->Pins[Pin]);
        if (!NT_SUCCESS(status)) return status;
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_WRITE);
        status = WdfIoTargetOpen(c->Pins[Pin], &open);
        if (!NT_SUCCESS(status)) { ClosePin(c, Pin); return status; }
        created = TRUE;
    }
    status = Transfer(c->Pins[Pin], TRUE, &Value);
    if (NT_SUCCESS(status)) {
        c->Configured |= 1u << Pin;
        c->Levels = (c->Levels & ~(1u << Pin)) | ((ULONG)Value << Pin);
    } else if (created) ClosePin(c, Pin);
    return status;
}
static VOID CloseAll(BOARD_CONTEXT *c)
{ ULONG pin; for (pin = 0; pin < PI5_BOARD_PINS; ++pin) ClosePin(c, pin); }

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT Object, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, BoardAdd);
    return WdfDriverCreate(Object, Path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS BoardAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG files;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_IO_QUEUE_CONFIG queue;
    WDFDEVICE device;
    NTSTATUS status;
    UNICODE_STRING name = RTL_CONSTANT_STRING(PI5_BOARD_NAME);
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\Pi5Board");
    UNREFERENCED_PARAMETER(Driver);
    status = WdfDeviceInitAssignName(Init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(Init, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (!NT_SUCCESS(status)) return status;
    WdfDeviceInitSetCharacteristics(Init, FILE_DEVICE_SECURE_OPEN, TRUE);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = BoardPrepare;
    pnp.EvtDeviceD0Entry = BoardStart; pnp.EvtDeviceD0Exit = BoardStop;
    pnp.EvtDeviceQueryRemove = BoardQueryRemove; pnp.EvtDeviceQueryStop = BoardQueryStop;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&files, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, BoardCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, BOARD_FILE); a.ExecutionLevel = WdfExecutionLevelPassive;
    WdfDeviceInitSetFileObjectConfig(Init, &files, &a);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, BOARD_CONTEXT); a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&Init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    BoardContext(device)->Device = device;
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = device;
    status = WdfWaitLockCreate(&a, &BoardContext(device)->Lock);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = BoardIo;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}
_Use_decl_annotations_
NTSTATUS BoardPrepare(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    BOARD_CONTEXT *c = BoardContext(Device);
    ULONG count = 0, i;
    ACPI_EVAL_INPUT_BUFFER input;
    ACPI_EVAL_OUTPUT_BUFFER output;
    WDF_MEMORY_DESCRIPTOR in, out;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Raw);
    RtlZeroMemory(&input, sizeof(input)); RtlZeroMemory(&output, sizeof(output));
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(input.MethodName, "_HRV", 4);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &input, sizeof(input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &output, sizeof(output));
    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device), NULL,
        IOCTL_ACPI_EVAL_METHOD, &in, &out, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (output.Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || output.Count != 1 ||
        output.Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER || output.Argument[0].DataLength != 4 ||
        output.Argument[0].Argument != 2) return STATUS_REVISION_MISMATCH;
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeConnection &&
            r->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
            r->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO) {
            if (count == PI5_BOARD_PINS) return STATUS_DEVICE_CONFIGURATION_ERROR;
            c->Connections[count].LowPart = r->u.Connection.IdLowPart;
            c->Connections[count++].HighPart = r->u.Connection.IdHighPart;
        }
    }
    return count == PI5_BOARD_PINS ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}
_Use_decl_annotations_
NTSTATUS BoardStart(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    BOARD_CONTEXT *c = BoardContext(Device);
    ULONG pin;
    NTSTATUS status = STATUS_SUCCESS;
    LARGE_INTEGER settle;
    UNREFERENCED_PARAMETER(Previous);
    WdfWaitLockAcquire(c->Lock, NULL);
    for (pin = 0; pin < PI5_BOARD_PINS; ++pin) {
        if (!((PI5_BOARD_ALWAYS_ON | c->Leased) & (1u << pin))) continue;
        status = SetPin(c, pin, (UCHAR)((PI5_BOARD_ALWAYS_ON & (1u << pin)) ? 1 : ((c->Levels >> pin) & 1)));
        if (!NT_SUCCESS(status)) break;
    }
    if (NT_SUCCESS(status)) {
        settle.QuadPart = -1500000; // Wi-Fi regulator settling, 150 ms.
        (void)KeDelayExecutionThread(KernelMode, FALSE, &settle);
        c->Online = TRUE;
    } else CloseAll(c);
    WdfWaitLockRelease(c->Lock);
    return status;
}
_Use_decl_annotations_
NTSTATUS BoardStop(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    BOARD_CONTEXT *c = BoardContext(Device);
    UNREFERENCED_PARAMETER(Target);
    WdfWaitLockAcquire(c->Lock, NULL);
    c->Online = FALSE; CloseAll(c);
    WdfWaitLockRelease(c->Lock);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS BoardQueryRemove(WDFDEVICE Device)
{
    BOARD_CONTEXT *c = BoardContext(Device);
    NTSTATUS status;
    WdfWaitLockAcquire(c->Lock, NULL);
    status = c->Leased ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    WdfWaitLockRelease(c->Lock);
    return status;
}
_Use_decl_annotations_
NTSTATUS BoardQueryStop(WDFDEVICE Device) { return BoardQueryRemove(Device); }
_Use_decl_annotations_
VOID BoardCleanup(WDFFILEOBJECT File)
{
    BOARD_CONTEXT *c = BoardContext(WdfFileObjectGetDevice(File));
    BOARD_FILE *f = BoardFile(File);
    ULONG pin;
    WdfWaitLockAcquire(c->Lock, NULL);
    for (pin = 0; pin < PI5_BOARD_PINS; ++pin) if (f->Leased & (1u << pin)) ClosePin(c, pin);
    c->Leased &= ~f->Leased; f->Leased = 0;
    WdfWaitLockRelease(c->Lock);
}
_Use_decl_annotations_
VOID BoardIo(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    BOARD_CONTEXT *c = BoardContext(WdfIoQueueGetDevice(Queue));
    BOARD_FILE *f;
    PI5_BOARD_SET *p;
    PI5_BOARD_STATUS *r;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;
    ULONG pin, bit;
    UCHAR value;
    UNREFERENCED_PARAMETER(OutLength);
    if (!WdfRequestGetFileObject(Request)) { WdfRequestComplete(Request, status); return; }
    f = BoardFile(WdfRequestGetFileObject(Request));
    WdfWaitLockAcquire(c->Lock, NULL);
    if (!c->Online) { status = STATUS_DEVICE_NOT_READY; goto done; }
    if (Code == IOCTL_PI5_BOARD_QUERY) {
        if (InLength) { status = STATUS_INVALID_PARAMETER; goto done; }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*r), (PVOID *)&r, NULL);
        if (!NT_SUCCESS(status)) goto done;
        RtlZeroMemory(r, sizeof(*r));
        r->Version = PI5_BOARD_VERSION; r->Size = sizeof(*r);
        r->Configured = c->Configured; r->Leased = c->Leased; r->AlwaysOn = PI5_BOARD_ALWAYS_ON;
        for (pin = 0; pin < PI5_BOARD_PINS; ++pin) if (c->Configured & (1u << pin)) {
            value = 0;
            status = Transfer(c->Pins[pin], FALSE, &value);
            if (!NT_SUCCESS(status)) goto done;
            r->Readable |= 1u << pin; r->Levels |= ((ULONG)value & 1) << pin;
        }
        information = sizeof(*r);
    } else if (Code == IOCTL_PI5_BOARD_SET || Code == IOCTL_PI5_BOARD_RELEASE) {
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*p), (PVOID *)&p, NULL);
        if (!NT_SUCCESS(status)) goto done;
        if (InLength != sizeof(*p) || p->Version != PI5_BOARD_VERSION || p->Pin >= PI5_BOARD_PINS ||
            p->Asserted > 1 || p->Reserved) { status = STATUS_INVALID_PARAMETER; goto done; }
        pin = p->Pin; bit = 1u << pin;
        if ((PI5_BOARD_ALWAYS_ON & bit) || (!(PI5_BOARD_LEDS & bit) && WdfRequestGetRequestorMode(Request) != KernelMode)) {
            status = STATUS_ACCESS_DENIED; goto done;
        }
        if ((c->Leased & bit) && !(f->Leased & bit)) { status = STATUS_SHARING_VIOLATION; goto done; }
        if (Code == IOCTL_PI5_BOARD_RELEASE) {
            if (f->Leased & bit) ClosePin(c, pin);
            f->Leased &= ~bit; c->Leased &= ~bit; status = STATUS_SUCCESS;
        } else {
            value = (UCHAR)((PI5_BOARD_LEDS & bit) ? !p->Asserted : p->Asserted);
            status = SetPin(c, pin, value);
            if (NT_SUCCESS(status)) { f->Leased |= bit; c->Leased |= bit; }
        }
    }
done:
    WdfWaitLockRelease(c->Lock);
    WdfRequestCompleteWithInformation(Request, status, NT_SUCCESS(status) ? information : 0);
}
