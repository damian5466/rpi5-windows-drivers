/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include <wmidata.h>
#include "hardware.h"
#include "public.h"

typedef struct DEVICE_CONTEXT {
    PUCHAR registers;
    ULONG length;
    unsigned kind;
    PI5_RNG_STATE rng;
    WDFWAITLOCK thermalLock;
    BOOLEAN thermalRunning;
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceContext)

typedef struct REQUEST_IO {
    DEVICE_CONTEXT *device;
    WDFREQUEST request;
    unsigned emptyPolls;
} REQUEST_IO;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Pi5DeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE Pi5PrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE Pi5ReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY Pi5D0Entry;
EVT_WDF_DEVICE_D0_EXIT Pi5D0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Pi5DeviceControl;
EVT_WDF_WMI_INSTANCE_QUERY_INSTANCE Pi5TemperatureQuery;

static const GUID ThermalGuid = MSAcpi_ThermalZoneTemperatureGuid;

static void DiagnosticValue(WDFDEVICE device, PCWSTR name, ULONG type,
                            ULONG length, PVOID value)
{
    WDFKEY key;
    UNICODE_STRING keyName;
    RtlInitUnicodeString(&keyName, name);
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE,
                    KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignValue(key, &keyName, type, length, value);
        WdfRegistryClose(key);
    }
}

static void Diagnostic(WDFDEVICE device, PCWSTR name, ULONG value)
{
    DiagnosticValue(device, name, REG_DWORD, sizeof(value), &value);
}

static NTSTATUS ResultStatus(enum PI5_RESULT result)
{
    switch (result) {
    case PI5_OK: return STATUS_SUCCESS;
    case PI5_INVALID: return STATUS_INVALID_PARAMETER;
    case PI5_NOT_READY: return STATUS_DEVICE_NOT_READY;
    case PI5_TIMEOUT: return STATUS_IO_TIMEOUT;
    case PI5_CANCELLED: return STATUS_CANCELLED;
    default: return STATUS_DEVICE_DATA_ERROR;
    }
}

static uint32_t Read32(void *context, uint32_t offset)
{
    REQUEST_IO *io = context;
    if (offset == PI5_RNG_FIFO_DATA) io->emptyPolls = 0;
    return READ_REGISTER_ULONG((PULONG)(io->device->registers + offset));
}

static uint64_t NowMs(void *context)
{
    UNREFERENCED_PARAMETER(context);
    return KeQueryInterruptTime() / 10000;
}

/* WMI callbacks are not power-managed I/O queue callbacks. Serialize sensor
 * access with D0 exit/release explicitly, and never return a cached reading. */
static NTSTATUS ReadTemperature(DEVICE_CONTEXT *ctx, uint32_t *raw, int32_t *temperature)
{
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    (void)WdfWaitLockAcquire(ctx->thermalLock, NULL);
    if (ctx->thermalRunning && ctx->registers) {
        *raw = READ_REGISTER_ULONG((PULONG)(ctx->registers + PI5_TEMP_STATUS));
        status = ResultStatus(pi5_temperature_decode(*raw, temperature));
    }
    WdfWaitLockRelease(ctx->thermalLock);
    return status;
}

_Use_decl_annotations_
NTSTATUS Pi5TemperatureQuery(WDFWMIINSTANCE instance, ULONG length,
                             PVOID buffer, PULONG used)
{
    DEVICE_CONTEXT *ctx = DeviceContext(WdfWmiInstanceGetDevice(instance));
    MSAcpi_ThermalZoneTemperature *data = buffer;
    uint32_t raw;
    int32_t temperature;
    NTSTATUS status;
    *used = sizeof(*data);
    if (length < sizeof(*data)) return STATUS_BUFFER_TOO_SMALL;
    *used = 0;
    status = ReadTemperature(ctx, &raw, &temperature);
    if (!NT_SUCCESS(status)) return status;
    RtlZeroMemory(data, sizeof(*data));
    /* The inbox WMI schema uses tenths of a kelvin, rounded to nearest.
     * Trip points/constants stay zero: this is a read-only monitor, not an
     * ACPI thermal policy that implements throttling or critical shutdown. */
    data->CurrentTemperature = (ULONG)((temperature + 273150 + 50) / 100);
    *used = sizeof(*data);
    return STATUS_SUCCESS;
}

static void WaitForFifo(void *context)
{
    REQUEST_IO *io = context;
    LARGE_INTEGER delay;
    /* A 1 ms sleep can round to a whole Windows scheduler tick, repeatedly
     * starving the small FIFO and timing out a 4 KiB read. Allow short polls
     * during normal generation; yield after 320 us with no progress. Never
     * change the system timer resolution. The core still enforces a deadline. */
    if (io->emptyPolls < 32) {
        ++io->emptyPolls;
        KeStallExecutionProcessor(10);
        return;
    }
    delay.QuadPart = -10000;
    (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

static int Cancelled(void *context)
{
    REQUEST_IO *io = context;
    return io->request && WdfRequestIsCanceled(io->request);
}

/* INF matching selects the function. Hardware-ID lists grow when firmware
 * supplies _HRV/_SUB; they are PnP identifiers, not a device data protocol. */
static NTSTATUS GetKind(WDFDEVICE device, unsigned *kind)
{
    WDFKEY key;
    ULONG value;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, L"DeviceKind");
    status = WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE,
                                    KEY_QUERY_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status)) return status;
    status = WdfRegistryQueryULong(key, &name, &value);
    WdfRegistryClose(key);
    if (!NT_SUCCESS(status)) return status;
    if (value != PI5_KIND_RNG && value != PI5_KIND_THERMAL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    *kind = value;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, Pi5DeviceAdd);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config,
                           WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS Pi5DeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG queue;
    WDFDEVICE device;
    unsigned kind;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init,
        FILE_DEVICE_SECURE_OPEN | FILE_AUTOGENERATED_DEVICE_NAME, TRUE);
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = Pi5PrepareHardware;
    pnp.EvtDeviceReleaseHardware = Pi5ReleaseHardware;
    pnp.EvtDeviceD0Entry = Pi5D0Entry;
    pnp.EvtDeviceD0Exit = Pi5D0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    status = GetKind(device, &kind);
    if (!NT_SUCCESS(status)) return status;
    DeviceContext(device)->kind = kind;
    if (kind == PI5_KIND_THERMAL) {
        WDF_WMI_PROVIDER_CONFIG provider;
        WDF_WMI_INSTANCE_CONFIG instance;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = device;
        status = WdfWaitLockCreate(&attributes, &DeviceContext(device)->thermalLock);
        if (!NT_SUCCESS(status)) return status;
        /* Reuse the Windows-supplied WMI class; no private MOF or client SDK. */
        WDF_WMI_PROVIDER_CONFIG_INIT(&provider, &ThermalGuid);
        provider.MinInstanceBufferSize = sizeof(MSAcpi_ThermalZoneTemperature);
        WDF_WMI_INSTANCE_CONFIG_INIT_PROVIDER_CONFIG(&instance, &provider);
        instance.Register = TRUE;
        instance.EvtWmiInstanceQueryInstance = Pi5TemperatureQuery;
        status = WdfWmiInstanceCreate(device, &instance, WDF_NO_OBJECT_ATTRIBUTES,
                                       WDF_NO_HANDLE);
        if (!NT_SUCCESS(status)) return status;
    }
    Diagnostic(device, L"DiagStage", 10);
    Diagnostic(device, L"DiagKind", kind);
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = Pi5DeviceControl;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_PI5_PLATFORM, NULL);
}

_Use_decl_annotations_
NTSTATUS Pi5PrepareHardware(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    DEVICE_CONTEXT *ctx = DeviceContext(device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory = NULL;
    ULONG i;
    UNREFERENCED_PARAMETER(raw);
    Diagnostic(device, L"DiagStage", 20);
    Diagnostic(device, L"DiagResourceCount", WdfCmResourceListGetCount(translated));
    for (i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR resource = WdfCmResourceListGetDescriptor(translated, i);
        if (!resource) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (i < 10) {
            WCHAR name[] = L"DiagResource0";
            name[12] = (WCHAR)(L'0' + i);
            DiagnosticValue(device, name, REG_BINARY, sizeof(*resource), resource);
        }
        if (resource->Type == CmResourceTypeMemory) {
            if (memory) return STATUS_DEVICE_CONFIGURATION_ERROR;
            memory = resource;
        /* Acpi.sys appends private bus metadata on Windows ARM64. It is not
         * another MMIO range and does not grant any hardware access. */
        } else if (resource->Type != CmResourceTypeNull &&
                   resource->Type != CmResourceTypeDevicePrivate)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (!memory || !pi5_resource_valid(ctx->kind,
        (uint64_t)memory->u.Memory.Start.QuadPart, memory->u.Memory.Length))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    ctx->length = memory->u.Memory.Length;
    Diagnostic(device, L"DiagStage", 30);
    /* No executable mapping; even an accidental kernel write cannot alter AVS. */
    ctx->registers = MmMapIoSpaceEx(memory->u.Memory.Start, ctx->length,
                                   PAGE_READONLY | PAGE_NOCACHE);
    if (!ctx->registers) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(&ctx->rng, sizeof(ctx->rng));
    Diagnostic(device, L"DiagStage", 40);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS Pi5ReleaseHardware(WDFDEVICE device, WDFCMRESLIST translated)
{
    DEVICE_CONTEXT *ctx = DeviceContext(device);
    UNREFERENCED_PARAMETER(translated);
    if (ctx->thermalLock) (void)WdfWaitLockAcquire(ctx->thermalLock, NULL);
    ctx->thermalRunning = FALSE;
    if (ctx->registers) {
        MmUnmapIoSpace(ctx->registers, ctx->length);
        ctx->registers = NULL;
    }
    if (ctx->thermalLock) WdfWaitLockRelease(ctx->thermalLock);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS Pi5D0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    DEVICE_CONTEXT *ctx = DeviceContext(device);
    REQUEST_IO requestIo = {ctx, NULL, 0};
    PI5_IO io = {&requestIo, Read32, NowMs, WaitForFifo, Cancelled};
    UNREFERENCED_PARAMETER(previous);
    if (!ctx->registers) return STATUS_DEVICE_NOT_READY;
    Diagnostic(device, L"DiagStage", 50);
    if (ctx->kind == PI5_KIND_THERMAL) {
        int32_t temperature;
        uint32_t raw = Read32(&requestIo, PI5_TEMP_STATUS);
        NTSTATUS status = ResultStatus(pi5_temperature_decode(raw, &temperature));
        (void)WdfWaitLockAcquire(ctx->thermalLock, NULL);
        ctx->thermalRunning = NT_SUCCESS(status);
        WdfWaitLockRelease(ctx->thermalLock);
        Diagnostic(device, L"DiagTemperatureRaw", raw);
        Diagnostic(device, L"DiagStatus", (ULONG)status);
        return status;
    } else {
        uint8_t sample[16];
        NTSTATUS status = ResultStatus(pi5_rng_read(&io, &ctx->rng, sample, sizeof(sample)));
        RtlSecureZeroMemory(sample, sizeof(sample));
        Diagnostic(device, L"DiagRngControl", Read32(&requestIo, PI5_RNG_CONTROL));
        Diagnostic(device, L"DiagRngStatus", Read32(&requestIo, PI5_RNG_STATUS));
        Diagnostic(device, L"DiagRngBitCount", Read32(&requestIo, PI5_RNG_BIT_COUNT));
        Diagnostic(device, L"DiagRngFifoCount", Read32(&requestIo, PI5_RNG_FIFO_COUNT));
        Diagnostic(device, L"DiagStatus", (ULONG)status);
        return status;
    }
}

_Use_decl_annotations_
NTSTATUS Pi5D0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    DEVICE_CONTEXT *ctx = DeviceContext(device);
    UNREFERENCED_PARAMETER(target);
    if (ctx->thermalLock) {
        (void)WdfWaitLockAcquire(ctx->thermalLock, NULL);
        ctx->thermalRunning = FALSE;
        WdfWaitLockRelease(ctx->thermalLock);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID Pi5DeviceControl(WDFQUEUE queue, WDFREQUEST request, size_t outputLength,
                     size_t inputLength, ULONG code)
{
    DEVICE_CONTEXT *ctx = DeviceContext(WdfIoQueueGetDevice(queue));
    REQUEST_IO requestIo = {ctx, request, 0};
    PI5_IO io = {&requestIo, Read32, NowMs, WaitForFifo, Cancelled};
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t information = 0;
    PVOID buffer = NULL;
    if (inputLength) { status = STATUS_INVALID_PARAMETER; goto complete; }
    if (!ctx->registers) { status = STATUS_DEVICE_NOT_READY; goto complete; }
    if (code == IOCTL_PI5_QUERY) {
        PI5_QUERY *query;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*query), &buffer, NULL);
        if (!NT_SUCCESS(status)) goto complete;
        query = buffer;
        RtlZeroMemory(query, sizeof(*query));
        query->size = sizeof(*query);
        query->version = PI5_ABI_VERSION;
        query->kind = ctx->kind;
        if (ctx->kind == PI5_KIND_THERMAL) {
            status = ReadTemperature(ctx, &query->temperature_raw,
                                      &query->temperature_millicelsius);
        } else {
            query->rng_control = Read32(&requestIo, PI5_RNG_CONTROL);
            query->rng_status = Read32(&requestIo, PI5_RNG_STATUS);
            query->rng_bit_count = Read32(&requestIo, PI5_RNG_BIT_COUNT);
            query->rng_fifo_count = Read32(&requestIo, PI5_RNG_FIFO_COUNT);
            query->rng_failed = (uint32_t)ctx->rng.failed;
        }
        if (NT_SUCCESS(status)) information = sizeof(*query);
    } else if (code == IOCTL_PI5_RANDOM && ctx->kind == PI5_KIND_RNG) {
        if (!outputLength || outputLength > PI5_RNG_MAX_BYTES) {
            status = STATUS_INVALID_BUFFER_SIZE;
            goto complete;
        }
        status = WdfRequestRetrieveOutputBuffer(request, outputLength, &buffer, NULL);
        if (!NT_SUCCESS(status)) goto complete;
        status = ResultStatus(pi5_rng_read(&io, &ctx->rng, buffer, outputLength));
        if (NT_SUCCESS(status)) information = outputLength;
    }
complete:
    WdfRequestCompleteWithInformation(request, status, information);
}
