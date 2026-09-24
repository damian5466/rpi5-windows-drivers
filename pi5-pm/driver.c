/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include "pi5-pm.h"
#include "hardware.h"

#define PM_BYTES 0x308u
#define PM_PHYSICAL 0x107d200000ULL

typedef struct {
    WDFDEVICE Device;
    WDFWAITLOCK Lock;
    PUCHAR Registers;
    WDFFILEOBJECT Owner;
    PM_HW_IO Io;
    PM_HW_STATE Hw;
    BOOLEAN Pinned;
    PI5_PM_STATUS Status;
} PM_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PM_CONTEXT, PmContext)

static volatile LONG BootUnsafe;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD PmAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PmPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE PmReleaseHardware;
EVT_WDF_DEVICE_QUERY_STOP PmQueryStop;
EVT_WDF_DEVICE_QUERY_REMOVE PmQueryRemove;
EVT_WDF_FILE_CLEANUP PmFileCleanup;
EVT_WDF_DEVICE_D0_ENTRY PmD0Entry;
EVT_WDF_DEVICE_D0_EXIT PmD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL PmControl;

static ULONG PmRead(PM_CONTEXT *c, ULONG offset)
{
    return READ_REGISTER_ULONG((PULONG)(c->Registers + offset)) & 0x00ffffffu;
}

static VOID PmSnapshot(PM_CONTEXT *c)
{
    if (c->Registers) {
        c->Status.ParentRaw = PmRead(c, PM_HW_GRAFX);
        c->Status.V3dRaw = PmRead(c, PM_HW_V3D);
        c->Status.GateAvailable =
            !!(c->Status.V3dRaw & PM_HW_V3D_ENAB) &&
            c->Status.V3dRaw != c->Status.ParentRaw;
        c->Status.RstcRaw = PmRead(c, 0x1c);
        c->Status.RstsRaw = PmRead(c, 0x20);
        c->Status.WdogRaw = PmRead(c, 0x24);
        c->Status.ImageRaw = PmRead(c, 0x108);
        c->Status.HdmiRaw = PmRead(c, 0x58);
        c->Status.UsbRaw = PmRead(c, 0x5c);
    }
    else c->Status.GateAvailable = 0;
    c->Status.Owner = c->Owner != NULL;
    c->Status.Unsafe = BootUnsafe != 0;
    c->Status.ParentUntouched = 1;
    c->Status.InitialV3dRaw = c->Hw.InitialV3d;
    c->Status.ExpectedV3dRaw = c->Hw.ExpectedV3d;
    c->Status.Transitions = c->Hw.Transitions;
}

static VOID PmFault(PM_CONTEXT *c, NTSTATUS status, ULONG operation)
{
    c->Status.LastStatus = (ULONG)status;
    c->Status.LastOperation = operation;
    ++c->Status.Failures;
    InterlockedExchange(&BootUnsafe, 1);
    c->Status.Unsafe = 1;
    if (!c->Pinned) {
        WdfDeviceSetStaticStopRemove(c->Device, FALSE);
        c->Pinned = TRUE;
    }
#if DBG
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "Pi5Pm: unsafe status=%08lx op=%lu parent=%08lx v3d=%08lx\n",
        (ULONG)status, operation, c->Status.ParentRaw, c->Status.V3dRaw);
#endif
}

static NTSTATUS PmLock(PM_CONTEXT *c)
{
    LONGLONG timeout = WDF_REL_TIMEOUT_IN_SEC(5);
    NTSTATUS status = WdfWaitLockAcquire(c->Lock, &timeout);
    return status == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT : status;
}

static uint32_t HwRead(void *context, uint32_t offset)
{
    PM_CONTEXT *c = context;
    return READ_REGISTER_ULONG((PULONG)(c->Registers + offset));
}

static void HwWrite(void *context, uint32_t offset, uint32_t value)
{
    PM_CONTEXT *c = context;
    WRITE_REGISTER_ULONG((PULONG)(c->Registers + offset), value);
}

static void HwDelay(void *context, uint32_t us)
{
    UNREFERENCED_PARAMETER(context);
    KeStallExecutionProcessor(us);
}

static NTSTATUS PmResult(PM_CONTEXT *c, PM_HW_RESULT result, ULONG operation)
{
    if (result == PmHwOk) {
        PmSnapshot(c);
        c->Status.LastStatus = STATUS_SUCCESS;
        c->Status.LastOperation = operation;
        return STATUS_SUCCESS;
    }
    if (result == PmHwNotReady) return STATUS_DEVICE_NOT_READY;
    if (result == PmHwInvalid) return STATUS_INVALID_DEVICE_STATE;
    PmSnapshot(c);
    PmFault(c, STATUS_DEVICE_HARDWARE_ERROR, operation);
    return STATUS_DEVICE_HARDWARE_ERROR;
}

static NTSTATUS PmAcquire(PM_CONTEXT *c, WDFFILEOBJECT file,
                          const PI5_PM_LEASE_REQUEST *request)
{
    NTSTATUS status;
    if (request->Version != PI5_PM_VERSION ||
        request->Domain != PI5_PM_V3D_DOMAIN ||
        request->Reset != PI5_PM_V3D_RESET || request->Reserved)
        return STATUS_INVALID_PARAMETER;
    if (!c->Registers || BootUnsafe) return STATUS_DEVICE_HARDWARE_ERROR;
    if (c->Owner) return STATUS_DEVICE_BUSY;
    status = PmResult(c, PmHwAcquire(&c->Io, &c->Hw), 0);
    if (!NT_SUCCESS(status)) return status;
    c->Owner = file;
    WdfDeviceSetStaticStopRemove(c->Device, FALSE);
    c->Pinned = TRUE;
    PmSnapshot(c);
    return STATUS_SUCCESS;
}

static NTSTATUS PmTransition(PM_CONTEXT *c, WDFFILEOBJECT file,
                             const PI5_PM_TRANSITION *request)
{
    PM_HW_RESULT result;
    if (request->Version != PI5_PM_VERSION ||
        request->Domain != PI5_PM_V3D_DOMAIN || request->Reserved)
        return STATUS_INVALID_PARAMETER;
    if (!c->Owner || c->Owner != file) return STATUS_ACCESS_DENIED;
    if (BootUnsafe) return STATUS_DEVICE_HARDWARE_ERROR;
    switch (request->Operation) {
    case Pi5PmV3dReleaseReset:
        result = PmHwSetV3dReset(&c->Io, &c->Hw, 1);
        break;
    case Pi5PmV3dAssertReset:
        result = PmHwSetV3dReset(&c->Io, &c->Hw, 0);
        break;
    case Pi5PmV3dPulseReset:
        if (!(c->Hw.ExpectedV3d & PM_HW_V3DRSTN)) return STATUS_INVALID_DEVICE_STATE;
        result = PmHwSetV3dReset(&c->Io, &c->Hw, 0);
        if (result == PmHwOk)
            result = PmHwSetV3dReset(&c->Io, &c->Hw, 1);
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }
    return PmResult(c, result, request->Operation);
}

static NTSTATUS PmDropLease(PM_CONTEXT *c, WDFFILEOBJECT file)
{
    NTSTATUS status;
    if (!c->Owner || c->Owner != file) return STATUS_ACCESS_DENIED;
    if (BootUnsafe) return STATUS_DEVICE_HARDWARE_ERROR;
    /* Caller must quiesce V3D and retain FCLK ID 5 until this succeeds. */
    status = PmResult(c, PmHwRelease(&c->Io, &c->Hw), 0);
    if (!NT_SUCCESS(status)) return status;
    c->Owner = NULL;
    if (c->Pinned) {
        WdfDeviceSetStaticStopRemove(c->Device, TRUE);
        c->Pinned = FALSE;
    }
    PmSnapshot(c);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, PmAdd);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS PmAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG files;
    WDF_IO_QUEUE_CONFIG queue;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, PI5_PM_NAME);
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\Pi5Pm");
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GR;;;BA)");
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, TRUE);
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WDF_FILEOBJECT_CONFIG_INIT(&files, WDF_NO_EVENT_CALLBACK,
                               WDF_NO_EVENT_CALLBACK, PmFileCleanup);
    WdfDeviceInitSetFileObjectConfig(init, &files, WDF_NO_OBJECT_ATTRIBUTES);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = PmPrepare;
    pnp.EvtDeviceReleaseHardware = PmReleaseHardware;
    pnp.EvtDeviceQueryStop = PmQueryStop;
    pnp.EvtDeviceQueryRemove = PmQueryRemove;
    pnp.EvtDeviceD0Entry = PmD0Entry;
    pnp.EvtDeviceD0Exit = PmD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PM_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    PmContext(device)->Device = device;
    PmContext(device)->Status.Version = PI5_PM_VERSION;
    PmContext(device)->Status.Debug = DBG;
    PmContext(device)->Io.Context = PmContext(device);
    PmContext(device)->Io.Read = HwRead;
    PmContext(device)->Io.Write = HwWrite;
    PmContext(device)->Io.DelayUs = HwDelay;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &PmContext(device)->Lock);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchParallel);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = PmControl;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    return WdfDeviceCreateSymbolicLink(device, &link);
}

_Use_decl_annotations_
NTSTATUS PmPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    PM_CONTEXT *c = PmContext(device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    ULONG i, count = WdfCmResourceListGetCount(translated), types = 0;
    WDFKEY key;
    DECLARE_CONST_UNICODE_STRING(countName, L"PmResourceCount");
    DECLARE_CONST_UNICODE_STRING(typesName, L"PmResourceTypes");
    DECLARE_CONST_UNICODE_STRING(descriptorName, L"PmMemoryResource");
    UNREFERENCED_PARAMETER(raw);
    if (BootUnsafe) return STATUS_DEVICE_HARDWARE_ERROR;
    for (i = 0; i < count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (r && r->Type < 32) types |= 1u << r->Type;
    }
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignULong(key, &countName, count);
        (void)WdfRegistryAssignULong(key, &typesName, types);
        WdfRegistryClose(key);
    }
    for (i = 0; i < count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        /* HAL/ACPI may append bookkeeping descriptors. They do not grant a
         * second hardware resource and are not mapped or accessed. */
        if (r->Type == CmResourceTypeNull || r->Type == CmResourceTypeDevicePrivate) continue;
        if (r->Type != CmResourceTypeMemory || memory) return STATUS_DEVICE_CONFIGURATION_ERROR;
        memory = r;
    }
    if (memory && NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignValue(key, &descriptorName, REG_BINARY, sizeof(*memory), memory);
        WdfRegistryClose(key);
    }
    if (!memory || memory->u.Memory.Length != PM_BYTES ||
        memory->u.Memory.Start.QuadPart != PM_PHYSICAL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->Registers = MmMapIoSpaceEx(memory->u.Memory.Start, PM_BYTES,
                                   PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->Registers) return STATUS_INSUFFICIENT_RESOURCES;
    PmSnapshot(c);
#if DBG
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "Pi5Pm: legacy=%08lx v3d=%08lx gate=%lu (read-only start)\n",
        c->Status.ParentRaw, c->Status.V3dRaw, c->Status.GateAvailable);
#endif
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PmReleaseHardware(WDFDEVICE device, WDFCMRESLIST translated)
{
    PM_CONTEXT *c = PmContext(device);
    UNREFERENCED_PARAMETER(translated);
    WdfWaitLockAcquire(c->Lock, NULL);
    if (c->Owner) {
        PmSnapshot(c);
        PmFault(c, STATUS_DEVICE_BUSY, 0);
    }
    c->Status.Online = 0;
    if (c->Registers) {
        MmUnmapIoSpace(c->Registers, PM_BYTES);
        c->Registers = NULL;
    }
    WdfWaitLockRelease(c->Lock);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PmQueryStop(WDFDEVICE device)
{
    PM_CONTEXT *c = PmContext(device);
    NTSTATUS status;
    WdfWaitLockAcquire(c->Lock, NULL);
    status = c->Owner || BootUnsafe ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    WdfWaitLockRelease(c->Lock);
    return status;
}

_Use_decl_annotations_
NTSTATUS PmQueryRemove(WDFDEVICE device)
{
    return PmQueryStop(device);
}

_Use_decl_annotations_
VOID PmFileCleanup(WDFFILEOBJECT file)
{
    PM_CONTEXT *c = PmContext(WdfFileObjectGetDevice(file));
    WdfWaitLockAcquire(c->Lock, NULL);
    /* A handle vanished without the consumer's quiesce/release contract.
     * Do not reset or power down a potentially active DMA master. */
    if (c->Owner == file) {
        if (c->Registers) PmSnapshot(c);
        PmFault(c, STATUS_DEVICE_HARDWARE_ERROR, 0);
        c->Owner = NULL;
        c->Status.Owner = 0;
    }
    WdfWaitLockRelease(c->Lock);
}

_Use_decl_annotations_
NTSTATUS PmD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    PM_CONTEXT *c = PmContext(device);
    UNREFERENCED_PARAMETER(previous);
    WdfWaitLockAcquire(c->Lock, NULL);
    c->Status.Online = c->Registers && !BootUnsafe;
    if (c->Registers) PmSnapshot(c);
    WdfWaitLockRelease(c->Lock);
    return BootUnsafe ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PmD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    PM_CONTEXT *c = PmContext(device);
    NTSTATUS status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(target);
    WdfWaitLockAcquire(c->Lock, NULL);
    c->Status.Online = 0;
    if (c->Owner) {
        PmSnapshot(c);
        PmFault(c, STATUS_DEVICE_BUSY, 0);
        status = STATUS_DEVICE_BUSY;
    }
    WdfWaitLockRelease(c->Lock);
    return status;
}

_Use_decl_annotations_
VOID PmControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength,
               size_t inputLength, ULONG code)
{
    PM_CONTEXT *c = PmContext(WdfIoQueueGetDevice(queue));
    WDFFILEOBJECT file = WdfRequestGetFileObject(request);
    NTSTATUS status;
    size_t written = 0;
    if ((code == IOCTL_PI5_PM_QUERY && inputLength != 0) ||
        (code == IOCTL_PI5_PM_RELEASE && (inputLength != sizeof(PI5_PM_LEASE_REQUEST) || outputLength != 0)) ||
        (code == IOCTL_PI5_PM_ACQUIRE && (inputLength != sizeof(PI5_PM_LEASE_REQUEST) || outputLength != 0)) ||
        (code == IOCTL_PI5_PM_TRANSITION && (inputLength != sizeof(PI5_PM_TRANSITION) || outputLength != 0))) {
        WdfRequestComplete(request, STATUS_INVALID_PARAMETER);
        return;
    }
    status = PmLock(c);
    if (!NT_SUCCESS(status)) goto Done;
    if (!c->Registers || !c->Status.Online) {
        status = STATUS_DEVICE_NOT_READY;
        goto Unlock;
    }
    if (code == IOCTL_PI5_PM_QUERY) {
        PI5_PM_STATUS *out;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status)) {
            PmSnapshot(c);
            *out = c->Status;
            written = sizeof(*out);
        }
    } else if (WdfRequestGetRequestorMode(request) != KernelMode || !file) {
        status = STATUS_ACCESS_DENIED;
    } else if (code == IOCTL_PI5_PM_ACQUIRE) {
        PI5_PM_LEASE_REQUEST *in;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*in), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status)) status = PmAcquire(c, file, in);
    } else if (code == IOCTL_PI5_PM_TRANSITION) {
        PI5_PM_TRANSITION *in;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*in), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status)) status = PmTransition(c, file, in);
    } else if (code == IOCTL_PI5_PM_RELEASE) {
        PI5_PM_LEASE_REQUEST *in;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*in), (PVOID *)&in, NULL);
        if (NT_SUCCESS(status)) {
            if (in->Version != PI5_PM_VERSION || in->Domain != PI5_PM_V3D_DOMAIN ||
                in->Reset != PI5_PM_V3D_RESET || in->Reserved)
                status = STATUS_INVALID_PARAMETER;
            else status = PmDropLease(c, file);
        }
    } else {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }
Unlock:
    WdfWaitLockRelease(c->Lock);
Done:
    WdfRequestCompleteWithInformation(request, status, written);
}
