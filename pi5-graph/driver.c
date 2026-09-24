/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>
#include "graph.h"

#define GRAPH_TAG 'gG5P'
static const GUID GraphUuid = {0xa95b0d30,0x818e,0x4a96,{0xa4,0x7b,0x72,0x6d,0x07,0x23,0x05,0x01}};
typedef struct {
    GRAPH Graph;
    WDFDEVICE Device;
    ULONGLONG Deadline;
    NTSTATUS TransportStatus;
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, Context)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AddDevice;
EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Control;

static void Diagnostic(WDFDEVICE device, PCWSTR name, ULONG value)
{
    WDFKEY key;
    UNICODE_STRING string;
    RtlInitUnicodeString(&string, name);
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(device, PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignULong(key, &string, value);
        WdfRegistryClose(key);
    }
}

static NTSTATUS Status(GRAPH_RESULT result)
{
    switch (result) {
    case GraphOk: return STATUS_SUCCESS;
    case GraphNoMemory: return STATUS_INSUFFICIENT_RESOURCES;
    case GraphNotFound: return STATUS_NOT_FOUND;
    case GraphTransport: return STATUS_IO_DEVICE_ERROR;
    default: return STATUS_DEVICE_DATA_ERROR;
    }
}

static void *Allocate(void *context, size_t bytes)
{
    UNREFERENCED_PARAMETER(context);
    return ExAllocatePool2(POOL_FLAG_PAGED, bytes, GRAPH_TAG);
}

static void Free(void *context, void *allocation)
{
    UNREFERENCED_PARAMETER(context);
    ExFreePoolWithTag(allocation, GRAPH_TAG);
}

/* Only the graph PDO's four read-only functions can be called. There is no
 * arbitrary AML/method passthrough, MMIO mapping, or mailbox ownership call. */
static GRAPH_RESULT Evaluate(void *context, uint32_t function, uint32_t count,
    const uint32_t *values, uint8_t *output, uint32_t capacity, uint32_t *used)
{
    DEVICE_CONTEXT *ctx = context;
    union { ULONGLONG Alignment; UCHAR Data[128]; } storage;
    PACPI_EVAL_INPUT_BUFFER_COMPLEX input = (PVOID)storage.Data;
    PACPI_METHOD_ARGUMENT a, child;
    WDF_MEMORY_DESCRIPTOR in, out;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG bytes, args, i;
    ULONG_PTR returned = 0;
    NTSTATUS status;
    *used = 0;
    if (function > 3 || count > 3) return GraphInvalid;
    if (KeQueryInterruptTime() >= ctx->Deadline) {
        ctx->TransportStatus = STATUS_IO_TIMEOUT;
        return GraphTransport;
    }
    args = ACPI_METHOD_ARGUMENT_LENGTH(sizeof(GUID)) +
        2 * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG)) +
        ACPI_METHOD_ARGUMENT_LENGTH(count * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG)));
    bytes = FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) + args;
    if (bytes > sizeof(storage)) return GraphInvalid;
    RtlZeroMemory(&storage, sizeof(storage));
    input->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    RtlCopyMemory(input->MethodName, "_DSM", 4);
    input->Size = args;
    input->ArgumentCount = 4;
    a = input->Argument;
    a->Type = ACPI_METHOD_ARGUMENT_BUFFER; a->DataLength = sizeof(GraphUuid);
    RtlCopyMemory(a->Data, &GraphUuid, sizeof(GraphUuid));
    a = ACPI_METHOD_NEXT_ARGUMENT(a); ACPI_METHOD_SET_ARGUMENT_INTEGER(a, 1);
    a = ACPI_METHOD_NEXT_ARGUMENT(a); ACPI_METHOD_SET_ARGUMENT_INTEGER(a, function);
    a = ACPI_METHOD_NEXT_ARGUMENT(a);
    a->Type = ACPI_METHOD_ARGUMENT_PACKAGE_EX;
    a->DataLength = (USHORT)(count * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG)));
    child = (PACPI_METHOD_ARGUMENT)a->Data;
    for (i = 0; i < count; ++i) {
        ACPI_METHOD_SET_ARGUMENT_INTEGER(child, values[i]);
        child = ACPI_METHOD_NEXT_ARGUMENT(child);
    }
    RtlZeroMemory(output, capacity);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, input, bytes);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, output, capacity);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(2));
    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(ctx->Device),
        NULL, IOCTL_ACPI_EVAL_METHOD, &in, &out, &options, &returned);
    ctx->TransportStatus = status;
    /* Contract maximum: two 1024-byte paths, or a 256-byte name + 4096-byte
     * chunk and scalar fields. 8192 covers every legal V1 response. Overflow
     * therefore indicates an incompatible contract, never truncated data. */
    if (status == STATUS_BUFFER_OVERFLOW || status == STATUS_BUFFER_TOO_SMALL) return GraphInvalid;
    if (!NT_SUCCESS(status)) return GraphTransport;
    if (returned > capacity) return GraphInvalid;
    *used = (uint32_t)returned;
    return GraphOk;
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
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, PI5_GRAPH_NAME);
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\Pi5Graph");
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, TRUE);
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = Prepare;
    pnp.EvtDeviceReleaseHardware = Release;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    Context(device)->Device = device;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfTrue;
    queue.EvtIoDeviceControl = Control;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    return WdfDeviceCreateSymbolicLink(device, &link);
}

_Use_decl_annotations_
NTSTATUS Prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    DEVICE_CONTEXT *ctx = Context(device);
    GRAPH_MEMORY memory = {NULL, Allocate, Free};
    GRAPH_SOURCE source = {ctx, Evaluate};
    GRAPH_RESULT result;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(raw);
    UNREFERENCED_PARAMETER(translated);
    ctx->TransportStatus = STATUS_SUCCESS;
    ctx->Deadline = KeQueryInterruptTime() + 30ull * 10000000ull;
    GraphFree(&ctx->Graph);
    result = GraphLoad(&ctx->Graph, &memory, &source);
    status = result == GraphTransport ? ctx->TransportStatus : Status(result);
    Diagnostic(device, L"GraphLoadStatus", (ULONG)status);
    Diagnostic(device, L"GraphNodes", ctx->Graph.Info.Nodes);
    Diagnostic(device, L"GraphProperties", ctx->Graph.Info.Properties);
    Diagnostic(device, L"GraphBytes", ctx->Graph.Info.PropertyBytes);
    Diagnostic(device, L"GraphDebug", DBG);
#if DBG
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "Pi5Graph: status=%08lx nodes=%lu properties=%lu bytes=%lu\n", (ULONG)status,
        (ULONG)ctx->Graph.Info.Nodes, (ULONG)ctx->Graph.Info.Properties,
        (ULONG)ctx->Graph.Info.PropertyBytes);
#endif
    return status;
}

_Use_decl_annotations_
NTSTATUS Release(WDFDEVICE device, WDFCMRESLIST translated)
{
    UNREFERENCED_PARAMETER(translated);
    GraphFree(&Context(device)->Graph);
    return STATUS_SUCCESS;
}

static void CopyNode(const GRAPH *graph, uint32_t index, PI5_GRAPH_NODE *out)
{
    const GRAPH_NODE *node = &graph->Nodes[index];
    RtlZeroMemory(out, sizeof(*out));
    out->Version = PI5_GRAPH_VERSION; out->Index = index;
    out->Parent = node->Parent; out->Properties = node->Count; out->Phandle = node->Phandle;
    RtlCopyMemory(out->Path, node->Path, strlen(node->Path) + 1);
    RtlCopyMemory(out->Owner, node->Owner, strlen(node->Owner) + 1);
}

_Use_decl_annotations_
VOID Control(WDFQUEUE queue, WDFREQUEST request, size_t outSize, size_t inSize, ULONG code)
{
    GRAPH *g = &Context(WdfIoQueueGetDevice(queue))->Graph;
    union { PI5_GRAPH_KEY Key; PI5_GRAPH_FIND Find; PI5_GRAPH_RESOLVE Resolve; } input;
    size_t neededIn, neededOut;
    PVOID in = NULL, out = NULL;
    uint32_t index;
    NTSTATUS status;
    if (!g->Nodes) { WdfRequestComplete(request, STATUS_DEVICE_NOT_READY); return; }
    switch (code) {
    case IOCTL_PI5_GRAPH_QUERY: neededIn = 0; neededOut = sizeof(PI5_GRAPH_INFO); break;
    case IOCTL_PI5_GRAPH_NODE: neededIn = sizeof(PI5_GRAPH_KEY); neededOut = sizeof(PI5_GRAPH_NODE); break;
    case IOCTL_PI5_GRAPH_PROPERTY: neededIn = sizeof(PI5_GRAPH_KEY); neededOut = sizeof(PI5_GRAPH_PROPERTY); break;
    case IOCTL_PI5_GRAPH_FIND: neededIn = sizeof(PI5_GRAPH_FIND); neededOut = sizeof(PI5_GRAPH_NODE); break;
    case IOCTL_PI5_GRAPH_RESOLVE: neededIn = sizeof(PI5_GRAPH_RESOLVE); neededOut = sizeof(PI5_GRAPH_REFERENCE); break;
    default: WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST); return;
    }
    if (inSize != neededIn) { WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return; }
    if (outSize < neededOut) { WdfRequestComplete(request, STATUS_BUFFER_TOO_SMALL); return; }
    RtlZeroMemory(&input, sizeof(input));
    if (neededIn) {
        status = WdfRequestRetrieveInputBuffer(request, neededIn, &in, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        RtlCopyMemory(&input, in, neededIn);
        if (input.Key.Version != PI5_GRAPH_VERSION) { WdfRequestComplete(request, STATUS_REVISION_MISMATCH); return; }
    }
    status = WdfRequestRetrieveOutputBuffer(request, neededOut, &out, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    RtlZeroMemory(out, neededOut);
    switch (code) {
    case IOCTL_PI5_GRAPH_QUERY:
        RtlCopyMemory(out, &g->Info, sizeof(g->Info));
        break;
    case IOCTL_PI5_GRAPH_NODE:
        if (input.Key.Node >= g->Info.Nodes || input.Key.Index || input.Key.Offset) status = STATUS_INVALID_PARAMETER;
        else CopyNode(g, input.Key.Node, out);
        break;
    case IOCTL_PI5_GRAPH_PROPERTY:
        if (input.Key.Node >= g->Info.Nodes || input.Key.Index >= g->Nodes[input.Key.Node].Count) status = STATUS_INVALID_PARAMETER;
        else {
            const GRAPH_PROPERTY *p = &g->Nodes[input.Key.Node].Properties[input.Key.Index];
            PI5_GRAPH_PROPERTY *o = out;
            if (input.Key.Offset > p->Length) { status = STATUS_INVALID_PARAMETER; break; }
            o->Version = PI5_GRAPH_VERSION; o->Node = input.Key.Node;
            o->Index = input.Key.Index; o->Offset = input.Key.Offset;
            o->Total = p->Length; o->Length = p->Length - input.Key.Offset;
            if (o->Length > PI5_GRAPH_CHUNK) o->Length = PI5_GRAPH_CHUNK;
            RtlCopyMemory(o->Name, p->Name, strlen(p->Name) + 1);
            if (o->Length) RtlCopyMemory(o->Data, p->Data + o->Offset, o->Length);
        }
        break;
    case IOCTL_PI5_GRAPH_FIND:
        status = Status(GraphFind(g, &input.Find, &index));
        if (NT_SUCCESS(status)) CopyNode(g, index, out);
        break;
    case IOCTL_PI5_GRAPH_RESOLVE:
        status = Status(GraphResolve(g, &input.Resolve, out));
        break;
    default: status = STATUS_INVALID_DEVICE_REQUEST; break;
    }
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? neededOut : 0);
}
