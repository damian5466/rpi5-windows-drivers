/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include <wmistr.h>
#include <wmidata.h>
#include "hardware.h"
#include "public.h"
#include "temperature.h"

typedef struct DEVICE_CONTEXT {
    PUCHAR registers;
    WDFWAITLOCK lock;
    WDFTIMER timer;
    WDFTIMER sensorTimer;
    WDFFILEOBJECT owner;
    BOOLEAN running;
    FAN_STATE state;
    FAN_POLICY policy;
    uint32_t requested;
    NTSTATUS temperatureStatus;
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD FanDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE FanPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE FanRelease;
EVT_WDF_DEVICE_D0_ENTRY FanD0Entry;
EVT_WDF_DEVICE_D0_EXIT FanD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL FanControl;
EVT_WDF_FILE_CLEANUP FanCleanup;
EVT_WDF_TIMER FanWatchdog;
EVT_WDF_TIMER FanTemperature;

static void Diagnostic(WDFDEVICE device, PCWSTR name, ULONG value)
{
    WDFKEY key;
    UNICODE_STRING keyName;
    RtlInitUnicodeString(&keyName, name);
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE,
                    KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignULong(key, &keyName, value);
        WdfRegistryClose(key);
    }
}
static NTSTATUS ResultStatus(enum FAN_RESULT result)
{
    switch (result) {
    case FAN_OK: return STATUS_SUCCESS;
    case FAN_INVALID: return STATUS_INVALID_PARAMETER;
    case FAN_NOT_READY: return STATUS_DEVICE_NOT_READY;
    default: return STATUS_DEVICE_DATA_ERROR;
    }
}
static uint32_t Read32(void *context, uint32_t offset)
{
    DEVICE_CONTEXT *ctx = context;
    return READ_REGISTER_ULONG((PULONG)(ctx->registers + offset));
}
static void Write32(void *context, uint32_t offset, uint32_t value)
{
    DEVICE_CONTEXT *ctx = context;
    WRITE_REGISTER_ULONG((PULONG)(ctx->registers + offset), value);
}
static uint64_t NowMs(void) { return KeQueryInterruptTime() / 10000; }

/* Query the same inbox WMI class that monitoring applications use. No sensor
 * device names, project GUIDs or private IOCTLs are part of this contract.
 * Run outside the fan lock: the independent watchdog still applies full
 * cooling if a provider stalls and the previous sample becomes stale. */
static NTSTATUS ReadTemperature(int32_t *temperature)
{
    static const GUID thermalGuid = MSAcpi_ThermalZoneTemperatureGuid;
    PVOID block = NULL, buffer = NULL;
    ULONG length = 0, capacity, attempt;
    NTSTATUS status;
    *temperature = INT32_MIN;
    status = IoWMIOpenBlock(&thermalGuid, WMIGUID_QUERY, &block);
    if (!NT_SUCCESS(status)) return status;
    status = IoWMIQueryAllData(block, &length, NULL);
    /* Providers can arrive between sizing and querying. Bound both retries
     * and nonpaged allocation even if a provider reports a corrupt size. */
    for (attempt = 0; status == STATUS_BUFFER_TOO_SMALL && attempt < 3; ++attempt) {
        if (length < sizeof(WNODE_ALL_DATA) || length > 64 * 1024) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        capacity = length;
        buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, capacity, 'Tm5P');
        if (!buffer) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
        status = IoWMIQueryAllData(block, &length, buffer);
        if (NT_SUCCESS(status) && (length > capacity ||
            !fan_temperature_parse(buffer, length, temperature)))
            status = STATUS_DEVICE_DATA_ERROR;
        ExFreePoolWithTag(buffer, 'Tm5P');
        buffer = NULL;
    }
    ObDereferenceObject(block);
    if (NT_SUCCESS(status) && *temperature == INT32_MIN) status = STATUS_DEVICE_NOT_READY;
    return status;
}

static NTSTATUS CheckIdentity(PWDFDEVICE_INIT init)
{
    WCHAR ids[256];
    ULONG used = 0;
    size_t at = 0, end, count;
    NTSTATUS status = WdfFdoInitQueryProperty(init, DevicePropertyCompatibleIDs,
                                            sizeof(ids), ids, &used);
    if (!NT_SUCCESS(status)) return status;
    if (used < 2 * sizeof(WCHAR) || used > sizeof(ids) || used % sizeof(WCHAR))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    count = used / sizeof(WCHAR);
    while (at < count && ids[at]) {
        UNICODE_STRING item, expected;
        for (end = at; end < count && ids[end]; ++end) { }
        if (end == count) return STATUS_DEVICE_CONFIGURATION_ERROR;
        item.Buffer = ids + at;
        item.Length = (USHORT)((end - at) * sizeof(WCHAR));
        item.MaximumLength = item.Length;
        RtlInitUnicodeString(&expected, L"ACPI\\RPI00F1");
        if (RtlEqualUnicodeString(&item, &expected, TRUE)) return STATUS_SUCCESS;
        at = end + 1;
    }
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, FanDeviceAdd);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS FanDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_FILEOBJECT_CONFIG file;
    WDF_TIMER_CONFIG timer;
    WDFDEVICE device;
    DEVICE_CONTEXT *ctx;
    NTSTATUS status = CheckIdentity(init);
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    UNREFERENCED_PARAMETER(driver);
    if (!NT_SUCCESS(status)) return status;
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN | FILE_AUTOGENERATED_DEVICE_NAME, TRUE);
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = FanPrepare;
    pnp.EvtDeviceReleaseHardware = FanRelease;
    pnp.EvtDeviceD0Entry = FanD0Entry;
    pnp.EvtDeviceD0Exit = FanD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&file, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, FanCleanup);
    WdfDeviceInitSetFileObjectConfig(init, &file, WDF_NO_OBJECT_ATTRIBUTES);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    ctx = DeviceContext(device);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &ctx->lock);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG_INIT(&timer, FanWatchdog);
    timer.AutomaticSerialization = FALSE;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timer, &attributes, &ctx->timer);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG_INIT(&timer, FanTemperature);
    timer.AutomaticSerialization = FALSE;
    status = WdfTimerCreate(&timer, &attributes, &ctx->sensorTimer);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = FanControl;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    Diagnostic(device, L"DiagStage", 10);
    return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_PI5_FAN, NULL);
}

_Use_decl_annotations_
NTSTATUS FanPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    DEVICE_CONTEXT *ctx = DeviceContext(device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    ULONG i;
    UNREFERENCED_PARAMETER(raw);
    Diagnostic(device, L"DiagStage", 20);
    for (i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeMemory) {
            if (memory) return STATUS_DEVICE_CONFIGURATION_ERROR;
            memory = r;
        } else if (r->Type != CmResourceTypeNull && r->Type != CmResourceTypeDevicePrivate &&
                   r->Type != CmResourceTypeInterrupt) return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (!memory || !fan_resource_valid((uint64_t)memory->u.Memory.Start.QuadPart, memory->u.Memory.Length))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    ctx->registers = MmMapIoSpaceEx(memory->u.Memory.Start, FAN_MMIO_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!ctx->registers) return STATUS_INSUFFICIENT_RESOURCES;
    Diagnostic(device, L"DiagStage", 40);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS FanRelease(WDFDEVICE device, WDFCMRESLIST translated)
{
    DEVICE_CONTEXT *ctx = DeviceContext(device);
    UNREFERENCED_PARAMETER(translated);
    (void)WdfWaitLockAcquire(ctx->lock, NULL);
    ctx->running = FALSE;
    ctx->owner = NULL;
    WdfWaitLockRelease(ctx->lock);
    /* Also handles failed D0Entry. Never wait while holding the callback lock. */
    (void)WdfTimerStop(ctx->timer, TRUE);
    (void)WdfTimerStop(ctx->sensorTimer, TRUE);
    (void)WdfWaitLockAcquire(ctx->lock, NULL);
    if (ctx->registers) {
        MmUnmapIoSpace(ctx->registers, FAN_MMIO_SIZE);
        ctx->registers = NULL;
    }
    WdfWaitLockRelease(ctx->lock);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS FanD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    DEVICE_CONTEXT *ctx = DeviceContext(device);
    FAN_IO io = {ctx, Read32, Write32};
    NTSTATUS status;
    UNREFERENCED_PARAMETER(previous);
    (void)WdfWaitLockAcquire(ctx->lock, NULL);
    RtlZeroMemory(&ctx->state, sizeof(ctx->state));
    fan_policy_start(&ctx->policy, NowMs());
    ctx->temperatureStatus = STATUS_DEVICE_NOT_READY;
    ctx->requested = 100;
    if (!ctx->registers) { WdfWaitLockRelease(ctx->lock); return STATUS_DEVICE_NOT_READY; }
    Diagnostic(device, L"DiagGlobal", Read32(ctx, FAN_GLOBAL));
    Diagnostic(device, L"DiagControl", Read32(ctx, FAN_CONTROL));
    Diagnostic(device, L"DiagRange", Read32(ctx, FAN_RANGE));
    Diagnostic(device, L"DiagPhase", Read32(ctx, FAN_PHASE));
    Diagnostic(device, L"DiagDuty", Read32(ctx, FAN_DUTY));
    Diagnostic(device, L"DiagRpm", Read32(ctx, FAN_RPM));
    status = ResultStatus(fan_full(&io, &ctx->state));
    Diagnostic(device, L"DiagStatus", (ULONG)status);
    Diagnostic(device, L"DiagStage", 50);
    if (NT_SUCCESS(status)) {
        ctx->running = TRUE;
        (void)WdfTimerStart(ctx->timer, WDF_REL_TIMEOUT_IN_MS(250));
        (void)WdfTimerStart(ctx->sensorTimer, WDF_REL_TIMEOUT_IN_MS(100));
    }
    WdfWaitLockRelease(ctx->lock);
    return status;
}

_Use_decl_annotations_
NTSTATUS FanD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    DEVICE_CONTEXT *ctx = DeviceContext(device);
    FAN_IO io = {ctx, Read32, Write32};
    UNREFERENCED_PARAMETER(target);
    (void)WdfWaitLockAcquire(ctx->lock, NULL);
    ctx->running = FALSE;
    if (ctx->registers) (void)fan_full(&io, &ctx->state);
    ctx->owner = NULL;
    WdfWaitLockRelease(ctx->lock);
    (void)WdfTimerStop(ctx->timer, TRUE);
    (void)WdfTimerStop(ctx->sensorTimer, TRUE);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID FanWatchdog(WDFTIMER timer)
{
    DEVICE_CONTEXT *ctx = DeviceContext((WDFDEVICE)WdfTimerGetParentObject(timer));
    FAN_IO io = {ctx, Read32, Write32};
    (void)WdfWaitLockAcquire(ctx->lock, NULL);
    if (ctx->running) {
        uint64_t now = NowMs();
        uint32_t target;
        BOOLEAN expired = ctx->state.deadline && now >= ctx->state.deadline;
        (void)fan_tick(&io, &ctx->state, now);
        if (expired) {
            ctx->owner = NULL;
            fan_policy_full(&ctx->policy, now);
        }
        target = fan_policy_target(&ctx->policy, now, Read32(ctx, FAN_RPM),
                                   ctx->state.deadline != 0, ctx->requested);
        (void)fan_adjust(&io, &ctx->state, target);
        (void)WdfTimerStart(timer, WDF_REL_TIMEOUT_IN_MS(250));
    }
    WdfWaitLockRelease(ctx->lock);
}

_Use_decl_annotations_
VOID FanTemperature(WDFTIMER timer)
{
    DEVICE_CONTEXT *ctx = DeviceContext((WDFDEVICE)WdfTimerGetParentObject(timer));
    int32_t temperature;
    uint64_t sampleTime;
    NTSTATUS status;
    (void)WdfWaitLockAcquire(ctx->lock, NULL);
    if (!ctx->running) { WdfWaitLockRelease(ctx->lock); return; }
    WdfWaitLockRelease(ctx->lock);
    sampleTime = NowMs();
    status = ReadTemperature(&temperature);
    (void)WdfWaitLockAcquire(ctx->lock, NULL);
    if (ctx->running) {
        /* A slow query must not make an old reading appear fresh. */
        fan_policy_temperature(&ctx->policy, temperature, sampleTime);
        ctx->temperatureStatus = status;
        (void)WdfTimerStart(timer, WDF_REL_TIMEOUT_IN_MS(1000));
    }
    WdfWaitLockRelease(ctx->lock);
}

_Use_decl_annotations_
VOID FanCleanup(WDFFILEOBJECT file)
{
    DEVICE_CONTEXT *ctx = DeviceContext(WdfFileObjectGetDevice(file));
    FAN_IO io = {ctx, Read32, Write32};
    (void)WdfWaitLockAcquire(ctx->lock, NULL);
    if (ctx->owner == file) {
        if (ctx->running) {
            (void)fan_full(&io, &ctx->state);
            fan_policy_full(&ctx->policy, NowMs());
        }
        ctx->owner = NULL;
    }
    WdfWaitLockRelease(ctx->lock);
}

_Use_decl_annotations_
VOID FanControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength,
                size_t inputLength, ULONG code)
{
    DEVICE_CONTEXT *ctx = DeviceContext(WdfIoQueueGetDevice(queue));
    FAN_IO io = {ctx, Read32, Write32};
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t information = 0;
    uint64_t now = NowMs();
    (void)WdfWaitLockAcquire(ctx->lock, NULL);
    if (!ctx->running) { status = STATUS_DEVICE_NOT_READY; goto done; }
    if (code == IOCTL_FAN_QUERY) {
        FAN_QUERY *q;
        if (inputLength) { status = STATUS_INVALID_PARAMETER; goto done; }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*q), (PVOID *)&q, NULL);
        if (!NT_SUCCESS(status)) goto done;
        RtlZeroMemory(q, sizeof(*q));
        q->size = sizeof(*q); q->version = FAN_ABI_VERSION;
        q->percent = ctx->state.percent;
        q->rpm = Read32(ctx, FAN_RPM);
        q->period_ns = FAN_PERIOD_NS;
        q->duty_ticks = Read32(ctx, FAN_DUTY);
        q->lease_remaining_ms = ctx->state.deadline > now ? (uint32_t)(ctx->state.deadline - now) : 0;
        q->expirations = ctx->state.expirations;
        q->failed = ctx->state.failed;
        q->global = Read32(ctx, FAN_GLOBAL); q->control = Read32(ctx, FAN_CONTROL);
        q->range = Read32(ctx, FAN_RANGE); q->phase = Read32(ctx, FAN_PHASE);
        q->manual = ctx->state.deadline != 0;
        q->requested_percent = q->manual ? ctx->requested : 0;
        q->temperature_millicelsius = ctx->policy.temperature;
        q->temperature_age_ms = now >= ctx->policy.temperature_time &&
            now - ctx->policy.temperature_time < UINT32_MAX ?
            (uint32_t)(now - ctx->policy.temperature_time) : UINT32_MAX;
        q->policy_flags = ctx->policy.flags;
        q->temperature_status = (uint32_t)ctx->temperatureStatus;
        information = sizeof(*q);
    } else if (code == IOCTL_FAN_SET) {
        FAN_SET *s;
        WDFFILEOBJECT file = WdfRequestGetFileObject(request);
        if (inputLength != sizeof(*s) || outputLength) { status = STATUS_INVALID_PARAMETER; goto done; }
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*s), (PVOID *)&s, NULL);
        if (!NT_SUCCESS(status)) goto done;
        if (s->size != sizeof(*s) || s->version != FAN_ABI_VERSION || !file ||
            s->percent > 100 || s->lease_ms < 500 || s->lease_ms > FAN_MAX_LEASE_MS) {
            status = STATUS_INVALID_PARAMETER; goto done;
        }
        if (ctx->owner && ctx->owner != file) { status = STATUS_DEVICE_BUSY; goto done; }
        status = ResultStatus(fan_set(&io, &ctx->state,
                    fan_policy_target(&ctx->policy, now, Read32(ctx, FAN_RPM), 1, s->percent),
                    s->lease_ms, now));
        if (NT_SUCCESS(status)) { ctx->owner = file; ctx->requested = s->percent; }
    }
done:
    WdfWaitLockRelease(ctx->lock);
    WdfRequestCompleteWithInformation(request, status, information);
}
