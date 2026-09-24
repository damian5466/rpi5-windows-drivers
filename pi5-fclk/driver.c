/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include "pi5-mailbox.h"
#include "pi5-fclk.h"

#define FCLK_IDS 5u
#define FCLK_MAILBOX_TIMEOUT_SEC 30u
typedef struct {
    WDFDEVICE Device;
    WDFWAITLOCK Lock;
    WDFIOTARGET Mailbox;
    ULONG Users[FCLK_IDS];
    ULONG BaselineRate, BaselineState;
    ULONG StateVote;
    ULONG StartupStatus, FaultStatus;
    ULONG LastControlOperation, LastControlRequested, LastControlReply;
    ULONG LastControlStatus, LastObservedState, LastObservedStatus;
    BOOLEAN Online, Claimed, Changed, Uncertain, Pinned, StateVoteValid;
} FCLK_CONTEXT;
typedef struct { ULONG Held; } FCLK_FILE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FCLK_CONTEXT, FclkContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FCLK_FILE, FclkFile)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD FclkAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE FclkPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE FclkReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY FclkStart;
EVT_WDF_DEVICE_D0_EXIT FclkStop;
EVT_WDF_DEVICE_QUERY_STOP FclkQueryStop;
EVT_WDF_DEVICE_QUERY_REMOVE FclkQueryRemove;
EVT_WDF_IO_TARGET_QUERY_REMOVE FclkMailboxQueryRemove;
EVT_WDF_FILE_CLEANUP FclkCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL FclkIo;

static const ULONG Ids[FCLK_IDS] = {
    PI5_FCLK_CORE, PI5_FCLK_V3D, PI5_FCLK_M2MC, PI5_FCLK_PIXEL_BVB, PI5_FCLK_DISP
};

static LONG Index(ULONG id)
{
    ULONG i;
    for (i = 0; i < FCLK_IDS; ++i) if (Ids[i] == id) return (LONG)i;
    return -1;
}

static BOOLEAN Busy(FCLK_CONTEXT *c)
{
    ULONG i;
    if (c->Uncertain || c->Claimed) return TRUE;
    for (i = 0; i < FCLK_IDS; ++i) if (c->Users[i]) return TRUE;
    return FALSE;
}

static VOID Pin(WDFDEVICE device, FCLK_CONTEXT *c)
{
    if (!c->Pinned) {
        WdfDeviceSetStaticStopRemove(device, FALSE);
        c->Pinned = TRUE;
    }
}

static VOID Unpin(WDFDEVICE device, FCLK_CONTEXT *c)
{
    if (c->Pinned && !Busy(c)) {
        WdfDeviceSetStaticStopRemove(device, TRUE);
        c->Pinned = FALSE;
    }
}

static VOID Diagnostic(WDFDEVICE device, FCLK_CONTEXT *c)
{
    WDFKEY key;
    DECLARE_CONST_UNICODE_STRING(debug, L"FclkDebug");
    DECLARE_CONST_UNICODE_STRING(startup, L"FclkStartupStatus");
    DECLARE_CONST_UNICODE_STRING(fault, L"FclkFaultStatus");
    DECLARE_CONST_UNICODE_STRING(operation, L"FclkLastControlOperation");
    DECLARE_CONST_UNICODE_STRING(requested, L"FclkLastControlRequested");
    DECLARE_CONST_UNICODE_STRING(reply, L"FclkLastControlReply");
    DECLARE_CONST_UNICODE_STRING(controlStatus, L"FclkLastControlStatus");
    DECLARE_CONST_UNICODE_STRING(observed, L"FclkLastObservedState");
    DECLARE_CONST_UNICODE_STRING(observedStatus, L"FclkLastObservedStatus");
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignULong(key, &debug, DBG);
        (void)WdfRegistryAssignULong(key, &startup, c->StartupStatus);
        (void)WdfRegistryAssignULong(key, &fault, c->FaultStatus);
        (void)WdfRegistryAssignULong(key, &operation, c->LastControlOperation);
        (void)WdfRegistryAssignULong(key, &requested, c->LastControlRequested);
        (void)WdfRegistryAssignULong(key, &reply, c->LastControlReply);
        (void)WdfRegistryAssignULong(key, &controlStatus, c->LastControlStatus);
        (void)WdfRegistryAssignULong(key, &observed, c->LastObservedState);
        (void)WdfRegistryAssignULong(key, &observedStatus, c->LastObservedStatus);
        WdfRegistryClose(key);
    }
}

static VOID Fault(WDFDEVICE device, FCLK_CONTEXT *c, NTSTATUS status)
{
    c->Uncertain = TRUE;
    c->FaultStatus = (ULONG)status;
    Pin(device, c);
    Diagnostic(device, c);
}

_Use_decl_annotations_
NTSTATUS FclkMailboxQueryRemove(WDFIOTARGET target)
{
    UNREFERENCED_PARAMETER(target);
    /* The open mailbox file carries clock ownership. Query-remove must not
     * close it while FCLK can still depend on that ownership. */
    return STATUS_UNSUCCESSFUL;
}

/* FCLK and MBX0 are independent ACPI devices. Opening on demand lets FCLK
 * start even if PnP has not started the mailbox provider yet. */
static NTSTATUS OpenMailbox(FCLK_CONTEXT *c)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_TARGET_OPEN_PARAMS open;
    UNICODE_STRING name = RTL_CONSTANT_STRING(PI5_MAILBOX_NAME);
    NTSTATUS status;
    if (c->Mailbox) return STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject = c->Device;
    status = WdfIoTargetCreate(c->Device, &attributes, &c->Mailbox);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_READ | GENERIC_WRITE);
    open.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    open.EvtIoTargetQueryRemove = FclkMailboxQueryRemove;
    status = WdfIoTargetOpen(c->Mailbox, &open);
    if (!NT_SUCCESS(status)) { WdfObjectDelete(c->Mailbox); c->Mailbox = NULL; }
    return status;
}

static NTSTATUS Query(FCLK_CONTEXT *c, ULONG operation, ULONG id, ULONG *value)
{
    PI5_MAILBOX_QUERY request = { PI5_MAILBOX_VERSION, 0, 0, 0 };
    PI5_MAILBOX_RESULT result = {0};
    WDF_MEMORY_DESCRIPTOR input, output;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR bytes = 0;
    NTSTATUS status;
    status = OpenMailbox(c);
    if (!NT_SUCCESS(status)) return status;
    request.Operation = operation; request.Id = id;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input, &request, sizeof(request));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &result, sizeof(result));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,
        WDF_REL_TIMEOUT_IN_SEC(FCLK_MAILBOX_TIMEOUT_SEC));
    status = WdfIoTargetSendIoctlSynchronously(c->Mailbox, NULL,
        IOCTL_PI5_MAILBOX_QUERY, &input, &output, &options, &bytes);
    if (!NT_SUCCESS(status)) return status;
    if (bytes != sizeof(result) || result.Version != PI5_MAILBOX_VERSION ||
        result.Operation != operation || result.Id != id)
        return STATUS_DEVICE_PROTOCOL_ERROR;
    *value = result.Value;
    return STATUS_SUCCESS;
}

static NTSTATUS Control(FCLK_CONTEXT *c, ULONG operation, ULONG value)
{
    PI5_MAILBOX_CLOCK_CONTROL request = { PI5_MAILBOX_VERSION, 0, PI5_FCLK_V3D, 0 };
    PI5_MAILBOX_RESULT result = {0};
    WDF_MEMORY_DESCRIPTOR input, output;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR bytes = 0;
    NTSTATUS status;
    c->LastControlOperation = operation;
    c->LastControlRequested = value;
    c->LastControlReply = ~0u;
    c->LastControlStatus = ~0u;
    status = OpenMailbox(c);
    if (!NT_SUCCESS(status)) {
        c->LastControlStatus = (ULONG)status;
        return status;
    }
    request.Operation = operation; request.Value = value;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input, &request, sizeof(request));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &result, sizeof(result));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,
        WDF_REL_TIMEOUT_IN_SEC(FCLK_MAILBOX_TIMEOUT_SEC));
    status = WdfIoTargetSendIoctlSynchronously(c->Mailbox, NULL,
        IOCTL_PI5_MAILBOX_CLOCK_CONTROL, &input, &output, &options, &bytes);
    c->LastControlStatus = (ULONG)status;
    if (!NT_SUCCESS(status)) return status;
    c->LastControlReply = result.Value;
    if (bytes != sizeof(result) || result.Version != PI5_MAILBOX_VERSION ||
        result.Operation != operation || result.Id != PI5_FCLK_V3D ||
        (operation == Pi5MailboxClockSetState && result.Value != value)) {
        c->LastControlStatus = (ULONG)STATUS_DEVICE_PROTOCOL_ERROR;
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS State(FCLK_CONTEXT *c, ULONG id, ULONG *state)
{
    NTSTATUS status = Query(c, Pi5MailboxClockState, id, state);
    if (NT_SUCCESS(status) && *state > 1u) return STATUS_DEVICE_CONFIGURATION_ERROR;
    return status;
}

static NTSTATUS Snapshot(FCLK_CONTEXT *c, ULONG id, PI5_FCLK_STATUS *out)
{
    NTSTATUS status;
    LONG index = Index(id);
    ULONG state, rate, minimum, maximum;
    if (index < 0) return STATUS_INVALID_PARAMETER;
    status = State(c, id, &state);
    if (!NT_SUCCESS(status)) return status;
    status = Query(c, Pi5MailboxClockRate, id, &rate);
    if (!NT_SUCCESS(status)) return status;
    status = Query(c, Pi5MailboxClockMinRate, id, &minimum);
    if (!NT_SUCCESS(status)) return status;
    status = Query(c, Pi5MailboxClockMaxRate, id, &maximum);
    if (!NT_SUCCESS(status)) return status;
    if (maximum && minimum > maximum) return STATUS_DEVICE_PROTOCOL_ERROR;
    RtlZeroMemory(out, sizeof(*out));
    out->Version = PI5_FCLK_VERSION; out->Id = id;
    out->State = state; out->RateHz = rate;
    out->MinHz = minimum; out->MaxHz = maximum;
    out->Users = c->Users[index];
    out->Flags = id == PI5_FCLK_V3D ? PI5_FCLK_FLAG_MUTABLE : PI5_FCLK_FLAG_READ_ONLY;
    if (id == PI5_FCLK_V3D && c->Claimed && c->StateVoteValid && c->StateVote)
        out->Flags |= PI5_FCLK_FLAG_STATE_REQUESTED;
    if (c->Uncertain) out->Flags |= PI5_FCLK_FLAG_UNCERTAIN;
    return STATUS_SUCCESS;
}

/* Called under Lock on the final V3D release. A failed write or verification
 * leaves mailbox ownership in place, since the hardware state is uncertain. */
static NTSTATUS Restore(WDFDEVICE device, FCLK_CONTEXT *c)
{
    ULONG value;
    NTSTATUS status;
    if (c->Uncertain) return STATUS_DEVICE_HARDWARE_ERROR;
    if (c->Changed) {
        if (c->BaselineRate) {
            status = Control(c, Pi5MailboxClockSetRate, c->BaselineRate);
            if (!NT_SUCCESS(status)) goto Uncertain;
        }
        status = Control(c, Pi5MailboxClockSetState, c->BaselineState);
        if (!NT_SUCCESS(status)) goto Uncertain;
        status = Query(c, Pi5MailboxClockRate, PI5_FCLK_V3D, &value);
        if (!NT_SUCCESS(status) || (c->BaselineRate && value != c->BaselineRate)) {
            status = STATUS_DEVICE_PROTOCOL_ERROR; goto Uncertain;
        }
        status = State(c, PI5_FCLK_V3D, &value);
        if (!NT_SUCCESS(status) || value != c->BaselineState) {
            status = STATUS_DEVICE_PROTOCOL_ERROR; goto Uncertain;
        }
    }
    status = Control(c, Pi5MailboxClockRelease, 0);
    if (!NT_SUCCESS(status)) goto Uncertain;
    c->Claimed = FALSE; c->Changed = FALSE;
    c->StateVote = 0; c->StateVoteValid = FALSE;
    c->BaselineRate = 0; c->BaselineState = 0;
    return STATUS_SUCCESS;
Uncertain:
    Fault(device, c, status);
    return status;
}

static NTSTATUS Acquire(WDFDEVICE device, FCLK_CONTEXT *c, FCLK_FILE *f,
    LONG index, PI5_FCLK_STATUS *out)
{
    NTSTATUS status;
    ULONG bit = 1u << index;
    if (f->Held & bit) return Snapshot(c, Ids[index], out);
    if (c->Uncertain) return STATUS_DEVICE_HARDWARE_ERROR;
    if (Ids[index] == PI5_FCLK_V3D) {
        if (c->Users[index]) return STATUS_DEVICE_BUSY;
        status = Control(c, Pi5MailboxClockClaim, 0);
        if (!NT_SUCCESS(status)) {
            /* A timed-out, cancelled, or malformed reply may hide a claim
             * that reached the mailbox provider. Keep this target pinned. */
            if (status == STATUS_IO_TIMEOUT || status == STATUS_CANCELLED ||
                status == STATUS_DEVICE_PROTOCOL_ERROR)
                Fault(device, c, status);
            return status;
        }
        c->Claimed = TRUE;
        c->StateVote = 0; c->StateVoteValid = FALSE;
        Pin(device, c);
        status = Snapshot(c, PI5_FCLK_V3D, out);
        if (!NT_SUCCESS(status)) {
            NTSTATUS released = Control(c, Pi5MailboxClockRelease, 0);
            if (!NT_SUCCESS(released)) Fault(device, c, released);
            else c->Claimed = FALSE;
            Unpin(device, c);
            return status;
        }
        c->BaselineRate = out->RateHz;
        c->BaselineState = out->State;
        c->Changed = FALSE;
    } else {
        status = Snapshot(c, Ids[index], out);
        if (!NT_SUCCESS(status)) return status;
    }
    ++c->Users[index]; f->Held |= bit; Pin(device, c);
    out->Users = c->Users[index];
    return STATUS_SUCCESS;
}

static NTSTATUS ReleaseLease(WDFDEVICE device, FCLK_CONTEXT *c, FCLK_FILE *f,
    LONG index)
{
    ULONG bit = 1u << index;
    NTSTATUS status = STATUS_SUCCESS;
    if (!(f->Held & bit)) return STATUS_INVALID_DEVICE_STATE;
    if (Ids[index] == PI5_FCLK_V3D && c->Users[index] == 1)
        status = Restore(device, c);
    if (!NT_SUCCESS(status)) return status;
    --c->Users[index]; f->Held &= ~bit;
    Unpin(device, c);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, FclkAdd);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS FclkAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG files;
    WDF_IO_QUEUE_CONFIG queue;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, PI5_FCLK_NAME);
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\Pi5Fclk");
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, TRUE);
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = FclkPrepare;
    pnp.EvtDeviceReleaseHardware = FclkReleaseHardware;
    pnp.EvtDeviceD0Entry = FclkStart; pnp.EvtDeviceD0Exit = FclkStop;
    pnp.EvtDeviceQueryStop = FclkQueryStop;
    pnp.EvtDeviceQueryRemove = FclkQueryRemove;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&files, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, FclkCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, FCLK_FILE);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    WdfDeviceInitSetFileObjectConfig(init, &files, &attributes);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, FCLK_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    FclkContext(device)->Device = device;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &FclkContext(device)->Lock);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = FclkIo;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    return WdfDeviceCreateSymbolicLink(device, &link);
}

_Use_decl_annotations_
NTSTATUS FclkPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(device); UNREFERENCED_PARAMETER(raw);
    UNREFERENCED_PARAMETER(translated);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS FclkReleaseHardware(WDFDEVICE device, WDFCMRESLIST translated)
{
    FCLK_CONTEXT *c = FclkContext(device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(translated);
    WdfWaitLockAcquire(c->Lock, NULL);
    status = Busy(c) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    if (NT_SUCCESS(status) && c->Mailbox) {
        WdfObjectDelete(c->Mailbox); c->Mailbox = NULL;
    }
    WdfWaitLockRelease(c->Lock);
    return status;
}

_Use_decl_annotations_
NTSTATUS FclkStart(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    FCLK_CONTEXT *c = FclkContext(device);
    PI5_FCLK_STATUS initial;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(previous);
    WdfWaitLockAcquire(c->Lock, NULL);
    c->Online = TRUE;
    /* Read-only initial discovery. PnP order may make MBX0 unavailable yet;
     * a client query/acquire retries OpenMailbox after its provider starts. */
    status = Snapshot(c, PI5_FCLK_V3D, &initial);
    c->StartupStatus = (ULONG)status;
    Diagnostic(device, c);
    WdfWaitLockRelease(c->Lock);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS FclkStop(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    FCLK_CONTEXT *c = FclkContext(device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(target);
    WdfWaitLockAcquire(c->Lock, NULL);
    status = Busy(c) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    if (NT_SUCCESS(status)) c->Online = FALSE;
    WdfWaitLockRelease(c->Lock);
    return status;
}

_Use_decl_annotations_
NTSTATUS FclkQueryStop(WDFDEVICE device)
{
    FCLK_CONTEXT *c = FclkContext(device);
    NTSTATUS status;
    WdfWaitLockAcquire(c->Lock, NULL);
    status = Busy(c) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    WdfWaitLockRelease(c->Lock);
    return status;
}

_Use_decl_annotations_
NTSTATUS FclkQueryRemove(WDFDEVICE device) { return FclkQueryStop(device); }

_Use_decl_annotations_
VOID FclkCleanup(WDFFILEOBJECT file)
{
    WDFDEVICE device = WdfFileObjectGetDevice(file);
    FCLK_CONTEXT *c = FclkContext(device);
    FCLK_FILE *f = FclkFile(file);
    ULONG i;
    WdfWaitLockAcquire(c->Lock, NULL);
    for (i = 0; i < FCLK_IDS; ++i) {
        if (!(f->Held & (1u << i))) continue;
        if (Ids[i] == PI5_FCLK_V3D && !c->Uncertain) {
            if (c->Changed) {
                /* The vanished consumer may still have DMA in flight. A
                 * deliberate RELEASE is its assertion that V3D is idle. */
                Fault(device, c, STATUS_DEVICE_HARDWARE_ERROR);
            } else {
                NTSTATUS status = Restore(device, c);
                if (!NT_SUCCESS(status)) Fault(device, c, status);
            }
        }
        --c->Users[i]; f->Held &= ~(1u << i);
    }
    Unpin(device, c);
    WdfWaitLockRelease(c->Lock);
}

_Use_decl_annotations_
VOID FclkIo(WDFQUEUE queue, WDFREQUEST request, size_t outputLength,
    size_t inputLength, ULONG code)
{
    WDFDEVICE device = WdfIoQueueGetDevice(queue);
    FCLK_CONTEXT *c = FclkContext(device);
    WDFFILEOBJECT file = WdfRequestGetFileObject(request);
    PI5_FCLK_REQUEST input, *buffer;
    PI5_FCLK_STATUS *output;
    NTSTATUS status;
    ULONG_PTR used = 0;
    LONG index;
    if (!file || inputLength != sizeof(input) || outputLength < sizeof(*output) ||
        (code != IOCTL_PI5_FCLK_QUERY && code != IOCTL_PI5_FCLK_ACQUIRE &&
         code != IOCTL_PI5_FCLK_RELEASE && code != IOCTL_PI5_FCLK_SET_RATE &&
         code != IOCTL_PI5_FCLK_SET_STATE)) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST); return;
    }
    if (code != IOCTL_PI5_FCLK_QUERY && WdfRequestGetRequestorMode(request) != KernelMode) {
        WdfRequestComplete(request, STATUS_ACCESS_DENIED); return;
    }
    status = WdfRequestRetrieveInputBuffer(request, sizeof(input), (PVOID *)&buffer, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    input = *buffer;
    if (input.Version != PI5_FCLK_VERSION || input.Reserved ||
        ((index = Index(input.Id)) < 0) ||
        ((code == IOCTL_PI5_FCLK_QUERY || code == IOCTL_PI5_FCLK_ACQUIRE ||
          code == IOCTL_PI5_FCLK_RELEASE) && input.Value)) {
        WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
    }
    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*output), (PVOID *)&output, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    WdfWaitLockAcquire(c->Lock, NULL);
    if (!c->Online) status = STATUS_DEVICE_NOT_READY;
    else if (code == IOCTL_PI5_FCLK_ACQUIRE) {
        status = Acquire(device, c, FclkFile(file), index, output);
        if (NT_SUCCESS(status)) used = sizeof(*output);
    } else if (code == IOCTL_PI5_FCLK_RELEASE) {
        ULONG baselineRate = c->BaselineRate, baselineState = c->BaselineState;
        status = Snapshot(c, input.Id, output);
        if (NT_SUCCESS(status)) status = ReleaseLease(device, c, FclkFile(file), index);
        if (NT_SUCCESS(status)) {
            if (input.Id == PI5_FCLK_V3D) {
                output->RateHz = baselineRate;
                output->State = baselineState;
                output->Flags &= ~PI5_FCLK_FLAG_STATE_REQUESTED;
            }
            output->Users = c->Users[index];
            used = sizeof(*output);
        }
    }
    else if (code == IOCTL_PI5_FCLK_SET_RATE || code == IOCTL_PI5_FCLK_SET_STATE) {
        PI5_FCLK_STATUS before;
        ULONG actual = 0;
        if (input.Id != PI5_FCLK_V3D || !(FclkFile(file)->Held & (1u << index)) ||
            c->Users[index] != 1) status = STATUS_ACCESS_DENIED;
        else if (c->Uncertain) status = STATUS_DEVICE_HARDWARE_ERROR;
        else if (code == IOCTL_PI5_FCLK_SET_STATE && input.Value > 1)
            status = STATUS_INVALID_PARAMETER;
        else {
            if (code == IOCTL_PI5_FCLK_SET_STATE) {
                c->LastObservedState = ~0u;
                c->LastObservedStatus = ~0u;
            }
            status = Snapshot(c, input.Id, &before);
            if (NT_SUCCESS(status) && code == IOCTL_PI5_FCLK_SET_RATE &&
                (!input.Value || !before.MaxHz || input.Value < before.MinHz ||
                 input.Value > before.MaxHz)) status = STATUS_INVALID_PARAMETER;
            if (NT_SUCCESS(status)) {
                c->Changed = TRUE; /* A failed transaction may still have reached firmware. */
                status = Control(c, code == IOCTL_PI5_FCLK_SET_RATE ?
                    Pi5MailboxClockSetRate : Pi5MailboxClockSetState, input.Value);
                if (NT_SUCCESS(status) && code == IOCTL_PI5_FCLK_SET_STATE) {
                    c->StateVote = input.Value;
                    c->StateVoteValid = TRUE;
                }
                if (NT_SUCCESS(status)) {
                    if (code == IOCTL_PI5_FCLK_SET_RATE)
                        status = Query(c, Pi5MailboxClockRate, input.Id, &actual);
                    else {
                        status = State(c, input.Id, &actual);
                        c->LastObservedStatus = (ULONG)status;
                        if (NT_SUCCESS(status)) c->LastObservedState = actual;
                    }
                }
                if (NT_SUCCESS(status) && code == IOCTL_PI5_FCLK_SET_RATE &&
                    (!actual || actual < before.MinHz || actual > before.MaxHz))
                    status = STATUS_DEVICE_PROTOCOL_ERROR;
                if (!NT_SUCCESS(status)) Fault(device, c, status);
            }
        }
    } else status = STATUS_SUCCESS;
    if (NT_SUCCESS(status) && !used) {
        status = Snapshot(c, input.Id, output);
        if (NT_SUCCESS(status)) used = sizeof(*output);
        else if (code == IOCTL_PI5_FCLK_SET_RATE || code == IOCTL_PI5_FCLK_SET_STATE)
            Fault(device, c, status);
    }
    if (NT_SUCCESS(status) && c->StartupStatus != (ULONG)STATUS_SUCCESS) {
        c->StartupStatus = (ULONG)STATUS_SUCCESS;
        Diagnostic(device, c);
    }
    WdfWaitLockRelease(c->Lock);
    WdfRequestCompleteWithInformation(request, status, used);
}
