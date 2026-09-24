/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>
#include <oprghdlr.h>
#include "mailbox.h"
#include "pi5-mailbox.h"

#define MBX_REG_BYTES 0x40u
#define MBX_REGION_SPACE 0x80u
static const GUID MailboxUuid = {0xa95b0d30,0x818e,0x4a96,{0xa4,0x7b,0x72,0x6d,0x07,0x23,0x05,0x02}};
/* A failed start after function 2 cannot give ownership back. Also reject a
 * second instance/restart within the same loaded driver, even on that path. */
static volatile LONG BootClaim;
typedef struct {
    WDFDEVICE Device;
    WDFWAITLOCK Lock;
    PUCHAR Registers;
    PDMA_ADAPTER Adapter;
    PHYSICAL_ADDRESS Address;
    MBX_TRANSPORT Transport;
    PVOID RegionObject;
    ULONG Region[6];
    PI5_MAILBOX_STATS Stats;
    volatile LONG RejectedRestarts;
    BOOLEAN Pinned;
    WDFFILEOBJECT ClockOwner;
    ULONG ClockBaselineRate, ClockBaselineState;
    BOOLEAN ClockAbandoned;
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, Context)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AddDevice;
EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
EVT_WDF_DEVICE_QUERY_STOP QueryStop;
EVT_WDF_DEVICE_QUERY_REMOVE QueryRemove;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Control;
EVT_WDF_FILE_CLEANUP ClockCleanup;
ACPI_OP_REGION_HANDLER RegionHandler;

static NTSTATUS Lock(DEVICE_CONTEXT *c)
{
    LONGLONG timeout = WDF_REL_TIMEOUT_IN_SEC(5);
    NTSTATUS status = WdfWaitLockAcquire(c->Lock, &timeout);
    /* STATUS_TIMEOUT is a success-severity value, but does not own the lock. */
    return status == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT : status;
}

static void Diagnostic(WDFDEVICE device, ULONG phase, NTSTATUS status)
{
    WDFKEY key;
    DECLARE_CONST_UNICODE_STRING(p, L"MailboxPhase");
    DECLARE_CONST_UNICODE_STRING(s, L"MailboxStatus");
    DECLARE_CONST_UNICODE_STRING(d, L"MailboxDebug");
    DECLARE_CONST_UNICODE_STRING(a, L"MailboxDmaAddress");
    DECLARE_CONST_UNICODE_STRING(h, L"MailboxDmaHigh");
    DECLARE_CONST_UNICODE_STRING(v, L"MailboxCpuAddress");
    DECLARE_CONST_UNICODE_STRING(ps, L"MailboxProbeStatus");
    DECLARE_CONST_UNICODE_STRING(pf, L"MailboxProbeFault");
    DECLARE_CONST_UNICODE_STRING(pu, L"MailboxProbeUnsafe");
    DECLARE_CONST_UNICODE_STRING(pa, L"MailboxEncodedAddress");
    DECLARE_CONST_UNICODE_STRING(pb, L"MailboxProbeBuffer");
    DECLARE_CONST_UNICODE_STRING(oc, L"MailboxOpRegionCalls");
    DECLARE_CONST_UNICODE_STRING(lr, L"MailboxLastReply");
    DECLARE_CONST_UNICODE_STRING(tc, L"MailboxTransactions");
    DECLARE_CONST_UNICODE_STRING(fr, L"MailboxForeignReplies");
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignULong(key, &p, phase);
        (void)WdfRegistryAssignULong(key, &s, (ULONG)status);
        (void)WdfRegistryAssignULong(key, &d, DBG);
        (void)WdfRegistryAssignULong(key, &a, Context(device)->Address.LowPart);
        (void)WdfRegistryAssignULong(key, &h, (ULONG)Context(device)->Address.HighPart);
        if (Context(device)->Transport.Buffer)
            (void)WdfRegistryAssignULong(key, &v, MmGetPhysicalAddress((PVOID)Context(device)->Transport.Buffer).LowPart);
        if (phase >= 5) {
            (void)WdfRegistryAssignULong(key, &ps, (ULONG)status);
            (void)WdfRegistryAssignULong(key, &pf, Context(device)->Transport.Fault);
            (void)WdfRegistryAssignULong(key, &pu, Context(device)->Transport.Unsafe);
            (void)WdfRegistryAssignULong(key, &pa, Context(device)->Transport.Address);
            (void)WdfRegistryAssignULong(key, &oc, Context(device)->Stats.OpRegionCalls);
            (void)WdfRegistryAssignULong(key, &lr, Context(device)->Transport.LastReply);
            (void)WdfRegistryAssignULong(key, &tc, Context(device)->Transport.Transactions);
            (void)WdfRegistryAssignULong(key, &fr, Context(device)->Transport.ForeignReplies);
            if (Context(device)->Transport.Buffer && !Context(device)->Transport.Unsafe) {
                ULONG words[MBX_BUFFER_BYTES / 4], i;
                for (i = 0; i < RTL_NUMBER_OF(words); ++i) words[i] = Context(device)->Transport.Buffer[i];
                (void)WdfRegistryAssignValue(key, &pb, REG_BINARY, sizeof(words), words);
            }
        }
        WdfRegistryClose(key);
    }
#if DBG
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "Pi5Mailbox: phase=%lu status=%08lx dma=%08lx\n", phase, (ULONG)status,
        (ULONG)Context(device)->Transport.Address);
#endif
}

/* Only the mailbox's fixed ownership DSM is accessible. Windows flattens its
 * package of scalar integers into the output argument array. Bound all fields. */
static NTSTATUS Evaluate(WDFDEVICE device, ULONG function, ULONG count, ULONG *values)
{
    union { ULONGLONG Align; UCHAR Bytes[96]; } inputStorage, outputStorage;
    PACPI_EVAL_INPUT_BUFFER_COMPLEX in = (PVOID)inputStorage.Bytes;
    PACPI_EVAL_OUTPUT_BUFFER out = (PVOID)outputStorage.Bytes;
    PACPI_METHOD_ARGUMENT arg;
    WDF_MEMORY_DESCRIPTOR inDesc, outDesc;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG args, bytes, i, at;
    ULONG_PTR returned = 0;
    NTSTATUS status;
    args = ACPI_METHOD_ARGUMENT_LENGTH(sizeof(GUID)) +
        3 * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG));
    bytes = FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) + args;
    RtlZeroMemory(&inputStorage, sizeof(inputStorage));
    RtlZeroMemory(&outputStorage, sizeof(outputStorage));
    in->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    RtlCopyMemory(in->MethodName, "_DSM", 4);
    in->Size = args; in->ArgumentCount = 4;
    arg = in->Argument;
    arg->Type = ACPI_METHOD_ARGUMENT_BUFFER; arg->DataLength = sizeof(MailboxUuid);
    RtlCopyMemory(arg->Data, &MailboxUuid, sizeof(MailboxUuid));
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg); ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, 1);
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg); ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, function);
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
    arg->Type = ACPI_METHOD_ARGUMENT_PACKAGE_EX; arg->DataLength = 0;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, in, bytes);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, out, sizeof(outputStorage));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(3));
    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(device), NULL,
        IOCTL_ACPI_EVAL_METHOD, &inDesc, &outDesc, &options, &returned);
    if (!NT_SUCCESS(status)) return status;
    if (returned < FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) || returned > sizeof(outputStorage) ||
        out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || out->Length != returned || out->Count != count)
        return STATUS_ACPI_INVALID_DATA;
    at = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument);
    for (i = 0; i < count; ++i) {
        if (returned - at < sizeof(ACPI_METHOD_ARGUMENT)) return STATUS_ACPI_INVALID_DATA;
        arg = (PVOID)(outputStorage.Bytes + at);
        if (arg->Type != ACPI_METHOD_ARGUMENT_INTEGER ||
            (arg->DataLength != 4 && arg->DataLength != 8) ||
            ACPI_METHOD_ARGUMENT_LENGTH(arg->DataLength) > returned - at)
            return STATUS_ACPI_INVALID_DATA;
        if (arg->DataLength == 8 && *(UNALIGNED ULONG *)(arg->Data + 4)) return STATUS_ACPI_INVALID_DATA;
        values[i] = arg->Argument;
        at += ACPI_METHOD_ARGUMENT_LENGTH(arg->DataLength);
    }
    return at == returned ? STATUS_SUCCESS : STATUS_ACPI_INVALID_DATA;
}

static uint32_t Read(void *context, uint32_t offset)
{
    DEVICE_CONTEXT *c = context;
    return READ_REGISTER_ULONG((PULONG)(c->Registers + offset));
}
static void Write(void *context, uint32_t offset, uint32_t value)
{
    DEVICE_CONTEXT *c = context;
    WRITE_REGISTER_ULONG((PULONG)(c->Registers + offset), value);
}
static uint64_t Now(void *context)
{
    UNREFERENCED_PARAMETER(context);
    return KeQueryInterruptTime();
}
static void Pause(void *context)
{
    LARGE_INTEGER interval;
    UNREFERENCED_PARAMETER(context);
    interval.QuadPart = -10000; /* 1 ms, yielding at PASSIVE_LEVEL. */
    (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
}
static void Barrier(void *context)
{
    UNREFERENCED_PARAMETER(context);
    KeMemoryBarrier();
}

static NTSTATUS Transfer(DEVICE_CONTEXT *c, ULONG tag, uint32_t *data, ULONG bytes)
{
    MBX_RESULT result;
    NTSTATUS status;
    ULONG id = data[0];
    if (!c->Stats.Online || !c->Stats.Owned) return STATUS_DEVICE_NOT_READY;
    result = MbxTransfer(&c->Transport, tag, data, bytes);
    switch (result) {
    case MbxOk: status = STATUS_SUCCESS; break;
    case MbxInvalid: status = STATUS_INVALID_PARAMETER; break;
    case MbxTimeout: status = STATUS_IO_TIMEOUT; break;
    case MbxFirmwareError: status = STATUS_IO_DEVICE_ERROR; break;
    default: status = STATUS_DEVICE_PROTOCOL_ERROR; break;
    }
    if (NT_SUCCESS(status) && bytes >= 8 && data[0] != id) {
        c->Transport.Fault = 1; ++c->Transport.Failures;
        status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    c->Stats.LastStatus = (ULONG)status; c->Stats.LastTag = tag;
    if (!NT_SUCCESS(status)) Diagnostic(c->Device, 9, status);
    return status;
}

_Use_decl_annotations_
NTSTATUS RegionHandler(ULONG access, PVOID object, ULONG address, ULONG size,
    PULONG data, ULONG_PTR context, PACPI_OP_REGION_CALLBACK completion, PVOID completionContext)
{
    DEVICE_CONTEXT *c = (PVOID)context;
    NTSTATUS status;
    uint32_t rtc[2] = {0,0};
    uint8_t *time = (PVOID)&c->Region[2];
    UNREFERENCED_PARAMETER(object);
    UNREFERENCED_PARAMETER(completion);
    UNREFERENCED_PARAMETER(completionContext);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (!data || size != 4 || (address & 3) || address > sizeof(c->Region) - 4 ||
        (access != ACPI_OPREGION_READ && access != ACPI_OPREGION_WRITE) ||
        (access == ACPI_OPREGION_WRITE && address == 4)) return STATUS_INVALID_PARAMETER;
    status = Lock(c);
    if (!NT_SUCCESS(status)) return status;
    ++c->Stats.OpRegionCalls;
    if (access == ACPI_OPREGION_READ) {
        *data = c->Region[address / 4];
    } else if (address) {
        c->Region[address / 4] = *data;
    } else {
        c->Region[0] = *data;
        c->Region[1] = 0xffffffffu;
        if (*data == 1) {
            RtlZeroMemory(time, 16);
            ++c->Stats.RtcReads;
            status = Transfer(c, 0x30087, rtc, sizeof(rtc));
            if (NT_SUCCESS(status)) {
                MbxEpochToTime(rtc[1], time);
                c->Stats.LastRtcEpoch = rtc[1];
            }
        } else if (*data == 2) {
            ++c->Stats.RtcWrites;
            c->Stats.LastRtcTimeZone = (SHORT)(time[10] | ((USHORT)time[11] << 8));
            status = MbxTimeToEpoch(time, &rtc[1]) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
            if (NT_SUCCESS(status)) status = Transfer(c, 0x38087, rtc, sizeof(rtc));
            if (NT_SUCCESS(status)) c->Stats.LastRtcSetEpoch = rtc[1];
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        if (NT_SUCCESS(status)) c->Region[1] = 0;
        else ++c->Stats.RtcFailures;
        /* Transfer failures are returned through the standard TAD status or
         * Valid byte, so AML does not fabricate a successful RTC update. */
    }
    WdfWaitLockRelease(c->Lock);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, AddDevice);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS AddDevice(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_FILEOBJECT_CONFIG files;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, PI5_MAILBOX_NAME);
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\Pi5Mailbox");
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, TRUE);
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = Prepare; pnp.EvtDeviceReleaseHardware = Release;
    pnp.EvtDeviceQueryStop = QueryStop; pnp.EvtDeviceQueryRemove = QueryRemove;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&files, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, ClockCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    WdfDeviceInitSetFileObjectConfig(init, &files, &attributes);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    Context(device)->Device = device;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &Context(device)->Lock);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchParallel);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = Control;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    return WdfDeviceCreateSymbolicLink(device, &link);
}

_Use_decl_annotations_
NTSTATUS Prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    DEVICE_CONTEXT *c = Context(device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    DEVICE_DESCRIPTION description = {0};
    ULONG values[6], i, interrupts = 0, maps = 0, phase = 1;
    PHYSICAL_ADDRESS physical, maximum;
    uint32_t rtc[2] = {0,0};
    NTSTATUS status;
    UNREFERENCED_PARAMETER(raw);
    c->Stats.Version = PI5_MAILBOX_VERSION; c->Stats.Debug = DBG;
    c->Region[1] = 0xffffffffu;
    status = Evaluate(device, 3, 6, values);
    if (!NT_SUCCESS(status)) goto Done;
    if (values[0] != 1 || values[1] != MBX_REGION_SPACE || values[2] != sizeof(c->Region) ||
        values[3] != 0 || values[4] != 0xc0000000u || values[5] != 0x40000000u) {
        status = STATUS_REVISION_MISMATCH; goto Done;
    }
    phase = 2;
    status = Evaluate(device, 1, 4, values);
    if (!NT_SUCCESS(status)) goto Done;
    if (values[0] != 1 || values[1] || values[3] || BootClaim) {
        status = STATUS_DEVICE_BUSY; goto Done;
    }
    phase = 3;
    for (i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (!r) { status = STATUS_DEVICE_CONFIGURATION_ERROR; goto Done; }
        if (r->Type == CmResourceTypeMemory) {
            if (memory || r->u.Memory.Length != MBX_REG_BYTES) {
                status = STATUS_DEVICE_CONFIGURATION_ERROR; goto Done;
            }
            memory = r;
        } else if (r->Type == CmResourceTypeInterrupt) {
            if (r->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED)) {
                status = STATUS_DEVICE_CONFIGURATION_ERROR; goto Done;
            }
            ++interrupts;
        }
    }
    if (!memory || interrupts != 1) { status = STATUS_DEVICE_CONFIGURATION_ERROR; goto Done; }
    c->Registers = MmMapIoSpaceEx(memory->u.Memory.Start, MBX_REG_BYTES, PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->Registers) { status = STATUS_INSUFFICIENT_RESOURCES; goto Done; }
    phase = 4;
    description.Version = DEVICE_DESCRIPTION_VERSION3;
    description.Master = TRUE; description.ScatterGather = TRUE;
    description.InterfaceType = Internal; description.DmaAddressWidth = 32;
    description.MaximumLength = PAGE_SIZE;
    c->Adapter = IoGetDmaAdapter(WdfDeviceWdmGetPhysicalDevice(device), &description, &maps);
    if (!c->Adapter) { status = STATUS_INSUFFICIENT_RESOURCES; goto Done; }
    c->Transport.Buffer = c->Adapter->DmaOperations->AllocateCommonBuffer(c->Adapter,
        PAGE_SIZE, &c->Address, FALSE);
    if (!c->Transport.Buffer) { status = STATUS_INSUFFICIENT_RESOURCES; goto Done; }
    physical = MmGetPhysicalAddress((PVOID)c->Transport.Buffer);
    if (c->Address.QuadPart == physical.QuadPart && physical.QuadPart > 0x40000000ll - PAGE_SIZE) {
        /* Windows 26200 returns untranslated addresses on this ACPI bus.
         * Ask HAL for a bounded, uncached common buffer; retain its address
         * for freeing. Encode the contract's VideoCore alias only after
         * proving the HAL mapping is identity. Never DMA to a CPU PA guessed
         * from a pool allocation, or silently truncate a wider address. */
        c->Adapter->DmaOperations->FreeCommonBuffer(c->Adapter, PAGE_SIZE,
            c->Address, (PVOID)c->Transport.Buffer, FALSE);
        c->Transport.Buffer = NULL;
        if (c->Adapter->DmaOperations->Size < FIELD_OFFSET(DMA_OPERATIONS, AllocateCommonBufferEx) +
            sizeof(c->Adapter->DmaOperations->AllocateCommonBufferEx) ||
            !c->Adapter->DmaOperations->AllocateCommonBufferEx) {
            status = STATUS_NOT_SUPPORTED; goto Done;
        }
        maximum.QuadPart = 0x3fffffff;
        c->Transport.Buffer = c->Adapter->DmaOperations->AllocateCommonBufferEx(c->Adapter,
            &maximum, PAGE_SIZE, &c->Address, FALSE, 0);
        if (!c->Transport.Buffer) { status = STATUS_INSUFFICIENT_RESOURCES; goto Done; }
        physical = MmGetPhysicalAddress((PVOID)c->Transport.Buffer);
    }
    if (!MbxEncodeAddress((uint64_t)c->Address.QuadPart, (uint64_t)physical.QuadPart,
        PAGE_SIZE, &c->Transport.Address) ||
        MmGetPhysicalAddress((PUCHAR)c->Transport.Buffer + PAGE_SIZE - 1).QuadPart != physical.QuadPart + PAGE_SIZE - 1) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR; goto Done;
    }
    for (i = 0; i < MBX_BUFFER_BYTES / 4; ++i) c->Transport.Buffer[i] = 0;
    c->Transport.Io.Context = c; c->Transport.Io.Read = Read; c->Transport.Io.Write = Write;
    c->Transport.Io.Now = Now; c->Transport.Io.Pause = Pause; c->Transport.Io.Barrier = Barrier;
    phase = 5;
    status = RegisterOpRegionHandler(WdfDeviceWdmGetPhysicalDevice(device), ACPI_OPREGION_ACCESS_AS_COOKED,
        MBX_REGION_SPACE, RegionHandler, c, 0, &c->RegionObject);
    if (!NT_SUCCESS(status)) goto Done;
    phase = 6;
    if (InterlockedCompareExchange(&BootClaim, 1, 0)) { status = STATUS_DEVICE_BUSY; goto Done; }
    WdfDeviceSetStaticStopRemove(device, FALSE); c->Pinned = TRUE;
    status = Evaluate(device, 2, 1, values);
    if (!NT_SUCCESS(status)) goto Done;
    if (values[0]) { status = STATUS_DEVICE_BUSY; goto Done; }
    phase = 7;
    status = Lock(c);
    if (!NT_SUCCESS(status)) goto Done;
    c->Stats.Owned = 1; c->Stats.Online = 1;
    /* Polling only: do not enable an unhandled mailbox interrupt. */
    Write(c, 0x1c, 0);
    status = Transfer(c, 0x30087, rtc, sizeof(rtc));
    if (NT_SUCCESS(status)) c->Stats.LastRtcEpoch = rtc[1];
    WdfWaitLockRelease(c->Lock);
    if (NT_SUCCESS(status)) phase = 8;
Done:
    Diagnostic(device, phase, status);
    return status;
}

_Use_decl_annotations_
NTSTATUS QueryStop(WDFDEVICE device)
{
    if (BootClaim) {
        InterlockedIncrement(&Context(device)->RejectedRestarts);
        return STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS QueryRemove(WDFDEVICE device) { return QueryStop(device); }

_Use_decl_annotations_
NTSTATUS Release(WDFDEVICE device, WDFCMRESLIST translated)
{
    DEVICE_CONTEXT *c = Context(device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(translated);
    /* Wait without a timeout during teardown: a live callback must finish
     * before its MMIO or common buffer can go away. Transactions are bounded. */
    (void)WdfWaitLockAcquire(c->Lock, NULL);
    c->Stats.Online = 0;
    WdfWaitLockRelease(c->Lock);
    if (c->RegionObject) {
        status = DeRegisterOpRegionHandler(WdfDeviceWdmGetPhysicalDevice(device), c->RegionObject);
        if (!NT_SUCCESS(status)) { Diagnostic(device, 90, status); return status; }
        c->RegionObject = NULL;
    }
    if (!c->Transport.Unsafe) {
        if (c->Transport.Buffer) c->Adapter->DmaOperations->FreeCommonBuffer(c->Adapter,
            PAGE_SIZE, c->Address, (PVOID)c->Transport.Buffer, FALSE);
        if (c->Adapter) c->Adapter->DmaOperations->PutDmaAdapter(c->Adapter);
    }
    /* An uncertain completion retains the entire allocation and DMA mapping
     * until reboot. A later start sees the latched firmware handoff request. */
    c->Transport.Buffer = NULL; c->Adapter = NULL;
    if (c->Registers) { MmUnmapIoSpace(c->Registers, MBX_REG_BYTES); c->Registers = NULL; }
    if (c->Pinned) { WdfDeviceSetStaticStopRemove(device, TRUE); c->Pinned = FALSE; }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void ClockCleanup(WDFFILEOBJECT file)
{
    DEVICE_CONTEXT *c = Context(WdfFileObjectGetDevice(file));
    (void)WdfWaitLockAcquire(c->Lock, NULL);
    if (c->ClockOwner == file) {
        /* The provider must explicitly release only after restoring baseline.
         * Never transfer an uncertain clock to a second controller. This is
         * separate from a mailbox transport fault so RTC service can continue. */
        c->ClockOwner = NULL;
        c->ClockAbandoned = TRUE;
    }
    WdfWaitLockRelease(c->Lock);
}

static NTSTATUS ClockControlLocked(DEVICE_CONTEXT *c, WDFFILEOBJECT file,
    const PI5_MAILBOX_CLOCK_CONTROL *q, PI5_MAILBOX_RESULT *r)
{
    uint32_t rate[2] = {5,0}, state[2] = {5,0}, data[3] = {5,0,0};
    uint32_t minimum[2] = {5,0}, maximum[2] = {5,0};
    NTSTATUS status;
    if (!c->Stats.Online || !c->Stats.Owned) return STATUS_DEVICE_NOT_READY;
    if (c->ClockAbandoned) return STATUS_DEVICE_HARDWARE_ERROR;
    if (q->Operation == Pi5MailboxClockClaim) {
        if (c->ClockOwner) return STATUS_DEVICE_BUSY;
        status = Transfer(c, 0x30002, rate, sizeof(rate));
        if (NT_SUCCESS(status)) status = Transfer(c, 0x30001, state, sizeof(state));
        if (!NT_SUCCESS(status)) return status;
        if (!rate[1] || state[1] > 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
        c->ClockBaselineRate = rate[1]; c->ClockBaselineState = state[1];
        c->ClockOwner = file;
    } else {
        if (c->ClockOwner != file) return STATUS_ACCESS_DENIED;
        if (q->Operation == Pi5MailboxClockRelease) {
            status = Transfer(c, 0x30002, rate, sizeof(rate));
            if (NT_SUCCESS(status)) status = Transfer(c, 0x30001, state, sizeof(state));
            if (!NT_SUCCESS(status)) return status;
            if (rate[1] != c->ClockBaselineRate || state[1] != c->ClockBaselineState)
                return STATUS_DEVICE_BUSY;
            c->ClockOwner = NULL;
        } else {
            data[1] = q->Value;
            if (q->Operation == Pi5MailboxClockSetRate) {
                status = Transfer(c, 0x30007, minimum, sizeof(minimum));
                if (NT_SUCCESS(status)) status = Transfer(c, 0x30004, maximum, sizeof(maximum));
                if (!NT_SUCCESS(status)) return status;
                if (!maximum[1] || minimum[1] > maximum[1] || q->Value < minimum[1] || q->Value > maximum[1])
                    return STATUS_INVALID_PARAMETER;
                status = Transfer(c, 0x38002, data, sizeof(data));
            } else {
                status = Transfer(c, 0x38001, data, 8);
            }
            if (!NT_SUCCESS(status)) return status;
        }
    }
    r->Version = PI5_MAILBOX_VERSION; r->Operation = q->Operation;
    r->Id = 5; r->Value = data[1];
    return STATUS_SUCCESS;
}

static void ClockControl(DEVICE_CONTEXT *c, WDFREQUEST request, size_t outputLength, size_t inputLength)
{
    PI5_MAILBOX_CLOCK_CONTROL q, *input;
    PI5_MAILBOX_RESULT *output;
    WDFFILEOBJECT file = WdfRequestGetFileObject(request);
    NTSTATUS status;
    if (WdfRequestGetRequestorMode(request) != KernelMode || !file) {
        WdfRequestComplete(request, STATUS_ACCESS_DENIED); return;
    }
    if (inputLength != sizeof(q) || outputLength < sizeof(*output)) {
        WdfRequestComplete(request, STATUS_INVALID_BUFFER_SIZE); return;
    }
    status = WdfRequestRetrieveInputBuffer(request, sizeof(q), (PVOID *)&input, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    q = *input;
    if (q.Version != PI5_MAILBOX_VERSION || q.Id != 5 ||
        q.Operation < Pi5MailboxClockClaim || q.Operation > Pi5MailboxClockSetState ||
        (q.Operation <= Pi5MailboxClockRelease && q.Value) ||
        (q.Operation == Pi5MailboxClockSetState && q.Value > 1) ||
        (q.Operation == Pi5MailboxClockSetRate && !q.Value)) {
        WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
    }
    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*output), (PVOID *)&output, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    status = Lock(c);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    status = ClockControlLocked(c, file, &q, output);
    WdfWaitLockRelease(c->Lock);
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? sizeof(*output) : 0);
}

_Use_decl_annotations_
void Control(WDFQUEUE queue, WDFREQUEST request, size_t outputLength, size_t inputLength, ULONG code)
{
    DEVICE_CONTEXT *c = Context(WdfIoQueueGetDevice(queue));
    PI5_MAILBOX_QUERY query = {0}, *input;
    PI5_MAILBOX_RESULT *output = NULL;
    PI5_MAILBOX_STATS *stats = NULL;
    uint32_t data[2] = {0,0};
    ULONG tag = 0, bytes = 8;
    size_t used = 0;
    NTSTATUS status;
    if (code == IOCTL_PI5_MAILBOX_CLOCK_CONTROL) {
        ClockControl(c, request, outputLength, inputLength); return;
    }
    if (code != IOCTL_PI5_MAILBOX_QUERY && code != IOCTL_PI5_MAILBOX_STATS) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST); return;
    }
    if (code == IOCTL_PI5_MAILBOX_QUERY) {
        if (inputLength != sizeof(query) || outputLength < sizeof(*output)) {
            WdfRequestComplete(request, STATUS_INVALID_BUFFER_SIZE); return;
        }
        status = WdfRequestRetrieveInputBuffer(request, sizeof(query), (PVOID *)&input, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        query = *input;
        if (query.Version != PI5_MAILBOX_VERSION || query.Reserved) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
        }
        switch (query.Operation) {
        case Pi5MailboxFirmwareRevision: if (!query.Id) { tag = 1; bytes = 4; } break;
        case Pi5MailboxClockRate: if (query.Id && query.Id <= 16) tag = 0x30002; break;
        case Pi5MailboxClockState: if (query.Id && query.Id <= 16) tag = 0x30001; break;
        case Pi5MailboxClockMinRate: if (query.Id && query.Id <= 16) tag = 0x30007; break;
        case Pi5MailboxClockMaxRate: if (query.Id && query.Id <= 16) tag = 0x30004; break;
        case Pi5MailboxRtcSeconds: if (!query.Id) tag = 0x30087; break;
        default: break;
        }
        if (!tag) { WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return; }
        data[0] = query.Id;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*output), (PVOID *)&output, NULL);
    } else {
        if (inputLength) { WdfRequestComplete(request, STATUS_INVALID_BUFFER_SIZE); return; }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*stats), (PVOID *)&stats, NULL);
    }
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    status = Lock(c);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    if (code == IOCTL_PI5_MAILBOX_QUERY) {
        status = Transfer(c, tag, data, bytes);
        if (NT_SUCCESS(status)) {
            output->Version = PI5_MAILBOX_VERSION; output->Operation = query.Operation;
            output->Id = query.Id; output->Value = data[bytes == 4 ? 0 : 1];
            used = sizeof(*output);
        }
    } else {
        *stats = c->Stats;
        stats->Fault = c->Transport.Fault; stats->Unsafe = c->Transport.Unsafe;
        stats->Transactions = c->Transport.Transactions; stats->Failures = c->Transport.Failures;
        stats->Timeouts = c->Transport.Timeouts; stats->ForeignReplies = c->Transport.ForeignReplies;
        stats->RejectedRestarts = (ULONG)c->RejectedRestarts;
        stats->DmaAddress = c->Transport.Address;
        used = sizeof(*stats);
    }
    WdfWaitLockRelease(c->Lock);
    WdfRequestCompleteWithInformation(request, status, used);
}
