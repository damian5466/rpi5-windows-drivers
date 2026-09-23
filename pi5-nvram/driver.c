/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntifs.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include "NvramFileLib.h"
#include "public.h"

static const GUID NvGuid = NV_FILE_GUID;
/* GUID_DEVINTERFACE_VOLUME; a Windows interface class, not a disk identity. */
static const GUID VolumeGuid = {0x53f5630d,0xb6bf,0x11d0,{0x94,0xf2,0x00,0xa0,0xc9,0x1e,0xfb,0x8b}};
static WDFDEVICE NvDevice;
typedef struct {
    WDFTIMER timer;
    WDFWAITLOCK lock;
    volatile LONG stopping;
    NV_FILE_INFO info;
    NV_FILE_IMAGE *snapshot, *copies;
    NV_DRIVER_QUERY query;
} NV_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NV_CONTEXT, NvContext)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD NvUnload;
EVT_WDF_TIMER NvTimer;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL NvControl;
EVT_WDF_DEVICE_SHUTDOWN_NOTIFICATION NvShutdown;

static NTSTATUS FirmwareGet(PCWSTR name, PVOID data, ULONG size)
{
    UNICODE_STRING variable;
    ULONG used = size, attributes = 0;
    GUID guid = NvGuid;
    NTSTATUS status;
    RtlInitUnicodeString(&variable, name);
    status = ExGetFirmwareEnvironmentVariable(&variable, &guid, data, &used, &attributes);
    if (NT_SUCCESS(status) && (used != size || attributes != 6)) return STATUS_DATA_ERROR;
    return status;
}
static NTSTATUS ReadInfo(NV_CONTEXT *ctx)
{
    NV_FILE_INFO info = {0};
    NTSTATUS status = FirmwareGet(NV_FILE_META_NAME, &info, sizeof(info));
    ctx->query.active = 0;
    if (!NT_SUCCESS(status)) return status;
    if (info.Size != sizeof(info) || info.Version != NV_FILE_VERSION || !info.Active ||
        !info.Sequence || info.Sequence < info.BootSequence) return STATUS_DEVICE_NOT_READY;
    if (ctx->info.Active && !RtlEqualMemory(ctx->info.StoreId, info.StoreId, 16)) return STATUS_DATA_ERROR;
    ctx->info = info;
    ctx->query.active = 1;
    ctx->query.current_sequence = info.Sequence;
    return STATUS_SUCCESS;
}

static NTSTATUS OpenExisting(PCWSTR volume, PCWSTR name, ACCESS_MASK access, HANDLE *file,
                             ULONGLONG expectedSize, PLARGE_INTEGER fileId)
{
    WCHAR path[768];
    UNICODE_STRING unicode;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK io = {0};
    FILE_STANDARD_INFORMATION standard;
    FILE_BASIC_INFORMATION basic;
    FILE_INTERNAL_INFORMATION internal;
    NTSTATUS status;
    *file = NULL;
    status = RtlStringCchCopyW(path, RTL_NUMBER_OF(path), volume);
    if (NT_SUCCESS(status)) status = RtlStringCchCatW(path, RTL_NUMBER_OF(path), L"\\");
    if (NT_SUCCESS(status)) status = RtlStringCchCatW(path, RTL_NUMBER_OF(path), name);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&unicode, path);
    InitializeObjectAttributes(&oa, &unicode, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    /* Reject intermediate filesystem junctions too: the files must stay on the
     * discovered volume. Object-manager volume interface links still resolve. */
    status = IoCreateFileEx(file, access | SYNCHRONIZE, &oa, &io, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
        FILE_OPEN_REPARSE_POINT | FILE_WRITE_THROUGH, NULL, 0,
        CreateFileTypeNone, NULL, IO_STOP_ON_SYMLINK, NULL);
    if (status == STATUS_STOPPED_ON_SYMLINK && io.Information) ExFreePool((PVOID)io.Information);
    if (!NT_SUCCESS(status)) { *file = NULL; return status; }
    status = ZwQueryInformationFile(*file, &io, &standard, sizeof(standard), FileStandardInformation);
    if (NT_SUCCESS(status) && (standard.Directory || standard.DeletePending ||
        (ULONGLONG)standard.EndOfFile.QuadPart != expectedSize)) status = STATUS_FILE_CORRUPT_ERROR;
    if (NT_SUCCESS(status)) {
        /* FileAttributeTagInformation is not supported by Windows FAT. Basic
         * information exposes the reparse attribute on FAT and NTFS alike. */
        status = ZwQueryInformationFile(*file, &io, &basic, sizeof(basic), FileBasicInformation);
        if (NT_SUCCESS(status) && (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) status = STATUS_REPARSE_POINT_NOT_RESOLVED;
    }
    if (NT_SUCCESS(status) && fileId) {
        status = ZwQueryInformationFile(*file, &io, &internal, sizeof(internal), FileInternalInformation);
        if (NT_SUCCESS(status)) *fileId = internal.IndexNumber;
    }
    if (!NT_SUCCESS(status)) { ZwClose(*file); *file = NULL; }
    return status;
}
static NTSTATUS ReadImage(HANDLE file, NV_FILE_IMAGE *image)
{
    IO_STATUS_BLOCK io;
    LARGE_INTEGER offset = {0};
    NTSTATUS status;
    RtlZeroMemory(image, sizeof(*image));
    status = ZwReadFile(file, NULL, NULL, NULL, &io, image, sizeof(*image), &offset, NULL);
    if (NT_SUCCESS(status) && io.Information != sizeof(*image)) return STATUS_DEVICE_DATA_ERROR;
    return status;
}
static NTSTATUS ImageName(const NV_FILE_HEADER *header, unsigned slot, WCHAR name[NV_FILE_PATH_SIZE])
{
    char path[NV_FILE_PATH_SIZE];
    unsigned i;
    if (!NvFileName(header, slot, path)) return STATUS_DATA_ERROR;
    for (i = 0; i < NV_FILE_PATH_SIZE; i++) {
        name[i] = path[i] == '/' ? L'\\' : (WCHAR)path[i];
        if (!path[i]) break;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS ReadPair(PCWSTR volume, const NV_FILE_HEADER *header, NV_FILE_IMAGE *copies)
{
    WCHAR name[NV_FILE_PATH_SIZE];
    unsigned i;
    NTSTATUS result = STATUS_OBJECT_NAME_NOT_FOUND;
    RtlZeroMemory(copies, 2 * NV_FILE_SIZE);
    for (i = 0; i < 2; i++) {
        HANDLE file = NULL;
        NTSTATUS status = ImageName(header, i, name);
        if (NT_SUCCESS(status)) status = OpenExisting(volume, name, FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                                       &file, NV_FILE_SIZE, NULL);
        if (NT_SUCCESS(status)) {
            status = ReadImage(file, &copies[i]);
            ZwClose(file);
        }
        if (!NT_SUCCESS(status)) RtlZeroMemory(&copies[i], NV_FILE_SIZE);
        else result = STATUS_SUCCESS;
    }
    return result;
}
static NTSTATUS FirmwareFilePresent(PCWSTR volume, const NV_FILE_HEADER *h)
{
    WCHAR name[128];
    HANDLE file;
    unsigned i;
    NTSTATUS status;
    for (i = 0; i < RTL_NUMBER_OF(name); i++) name[i] = h->FirmwarePath[i] == '/' ? L'\\' : (WCHAR)h->FirmwarePath[i];
    status = OpenExisting(volume, name, FILE_READ_DATA | FILE_READ_ATTRIBUTES, &file, h->FirmwareSize, NULL);
    if (NT_SUCCESS(status)) ZwClose(file);
    return status;
}
static NTSTATUS Discover(NV_CONTEXT *ctx, WCHAR selectedVolume[512])
{
    PWSTR links = NULL, at;
    NTSTATUS status = IoGetDeviceInterfaces(&VolumeGuid, NULL, 0, &links);
    unsigned matches = 0;
    selectedVolume[0] = 0;
    ctx->query.matching_volumes = 0;
    if (!NT_SUCCESS(status)) return status;
    for (at = links; at && *at;) {
        size_t length;
        int selected;
        status = RtlStringCchLengthW(at, 512, &length);
        if (!NT_SUCCESS(status)) { status = STATUS_NAME_TOO_LONG; goto Done; }
        status = ReadPair(at, &ctx->snapshot->Header, ctx->copies);
        selected = NT_SUCCESS(status) ? NvFileSelect(&ctx->copies[0], &ctx->copies[1]) : -1;
        if (selected >= 0 && NvFileSameStore(&ctx->snapshot->Header, &ctx->copies[selected].Header) &&
            NT_SUCCESS(FirmwareFilePresent(at, &ctx->snapshot->Header))) {
            matches++;
            if (matches == 1) {
                status = RtlStringCchCopyW(selectedVolume, 512, at);
                if (!NT_SUCCESS(status)) goto Done;
            }
        }
        at += length + 1;
    }
    status = matches == 1 ? STATUS_SUCCESS : matches ? STATUS_DUPLICATE_OBJECTID : STATUS_OBJECT_NAME_NOT_FOUND;
Done:
    ctx->query.matching_volumes = matches;
    if (links) ExFreePool(links);
    return status;
}
static NTSTATUS WritePart(HANDLE file, PVOID data, ULONG size, LONGLONG position)
{
    IO_STATUS_BLOCK io;
    LARGE_INTEGER offset;
    NTSTATUS status;
    offset.QuadPart = position;
    status = ZwWriteFile(file, NULL, NULL, NULL, &io, data, size, &offset, NULL);
    if (NT_SUCCESS(status) && io.Information != size) return STATUS_DEVICE_DATA_ERROR;
    if (NT_SUCCESS(status)) status = ZwFlushBuffersFile(file, &io);
    return status;
}
static NTSTATUS Commit(NV_CONTEXT *ctx, PCWSTR volume)
{
    HANDLE files[2] = {NULL, NULL};
    LARGE_INTEGER ids[2];
    NTSTATUS status;
    int selected, target;
    WCHAR name[NV_FILE_PATH_SIZE];
    unsigned i;
    /* Open both without write-sharing, then revalidate after discovery. */
    for (i = 0; i < 2; i++) {
        status = ImageName(&ctx->snapshot->Header, i, name);
        if (NT_SUCCESS(status)) status = OpenExisting(volume, name, FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES,
                               &files[i], NV_FILE_SIZE, &ids[i]);
        if (!NT_SUCCESS(status)) goto Done;
        status = ReadImage(files[i], &ctx->copies[i]);
        if (!NT_SUCCESS(status)) goto Done;
    }
    if (ids[0].QuadPart == ids[1].QuadPart) { status = STATUS_DUPLICATE_OBJECTID; goto Done; }
    selected = NvFileSelect(&ctx->copies[0], &ctx->copies[1]);
    if (selected < 0 || !NvFileSameStore(&ctx->snapshot->Header, &ctx->copies[selected].Header)) {
        status = STATUS_FILE_CORRUPT_ERROR; goto Done;
    }
    if (ctx->copies[selected].Header.Sequence > ctx->snapshot->Header.Sequence) {
        status = STATUS_REVISION_MISMATCH; goto Done;
    }
    if (ctx->copies[selected].Header.Sequence == ctx->snapshot->Header.Sequence) {
        status = RtlEqualMemory(&ctx->copies[selected], ctx->snapshot, NV_FILE_SIZE) ? STATUS_SUCCESS : STATUS_DATA_ERROR;
        goto Done;
    }
    target = selected ^ 1;
    status = WritePart(files[target], ctx->snapshot->Data, NV_FILE_DATA_SIZE, NV_FILE_HEADER_SIZE);
    if (NT_SUCCESS(status)) status = WritePart(files[target], &ctx->snapshot->Header, NV_FILE_HEADER_SIZE, 0);
    if (NT_SUCCESS(status)) status = ReadImage(files[target], &ctx->copies[target]);
    if (NT_SUCCESS(status) && (!NvFileValid(&ctx->copies[target], NV_FILE_SIZE) ||
        !RtlEqualMemory(&ctx->copies[target], ctx->snapshot, NV_FILE_SIZE))) status = STATUS_DEVICE_DATA_ERROR;
    if (NT_SUCCESS(status)) ctx->query.saves++;
Done:
    for (i = 0; i < 2; i++) if (files[i]) ZwClose(files[i]);
    return status;
}
static NTSTATUS Save(NV_CONTEXT *ctx, BOOLEAN force)
{
    WCHAR volume[512];
    NTSTATUS status = ReadInfo(ctx);
    if (!NT_SUCCESS(status)) goto Done;
    if (!force && ctx->query.saved_sequence == ctx->info.Sequence) return STATUS_SUCCESS;
    status = FirmwareGet(NV_FILE_SNAPSHOT_NAME, ctx->snapshot, NV_FILE_SIZE);
    if (!NT_SUCCESS(status)) goto Done;
    if (!NvFileValid(ctx->snapshot, NV_FILE_SIZE) ||
        !RtlEqualMemory(ctx->snapshot->Header.StoreId, ctx->info.StoreId, 16) ||
        ctx->snapshot->Header.Sequence < ctx->info.Sequence) { status = STATUS_DATA_ERROR; goto Done; }
    ctx->query.current_sequence = ctx->snapshot->Header.Sequence;
    status = Discover(ctx, volume);
    if (NT_SUCCESS(status)) status = Commit(ctx, volume);
    if (NT_SUCCESS(status)) {
        ctx->query.saved_sequence = ctx->snapshot->Header.Sequence;
        (void)RtlStringCchCopyW(ctx->query.volume, RTL_NUMBER_OF(ctx->query.volume), volume);
    }
Done:
    ctx->query.last_status = (unsigned int)status;
    if (!NT_SUCCESS(status)) ctx->query.failures++;
    return status;
}

_Use_decl_annotations_
VOID NvTimer(WDFTIMER timer)
{
    NV_CONTEXT *ctx = NvContext(WdfTimerGetParentObject(timer));
    WdfWaitLockAcquire(ctx->lock, NULL);
    if (!InterlockedCompareExchange(&ctx->stopping, 0, 0)) (void)Save(ctx, FALSE);
    WdfWaitLockRelease(ctx->lock);
    if (!InterlockedCompareExchange(&ctx->stopping, 0, 0)) WdfTimerStart(timer, WDF_REL_TIMEOUT_IN_MS(1000));
}
_Use_decl_annotations_
VOID NvControl(WDFQUEUE queue, WDFREQUEST request, size_t outputSize, size_t inputSize, ULONG code)
{
    NV_CONTEXT *ctx = NvContext(WdfIoQueueGetDevice(queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    NV_DRIVER_QUERY *output;
    ULONG_PTR used = 0;
    UNREFERENCED_PARAMETER(outputSize);
    WdfWaitLockAcquire(ctx->lock, NULL);
    if (inputSize) status = STATUS_INVALID_PARAMETER;
    else if (code == IOCTL_NV_QUERY) {
        (void)ReadInfo(ctx);
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*output), (PVOID *)&output, NULL);
        if (NT_SUCCESS(status)) { *output = ctx->query; used = sizeof(*output); }
    } else if (code == IOCTL_NV_FLUSH) {
        status = ctx->stopping ? STATUS_DELETE_PENDING : Save(ctx, TRUE);
    }
    WdfWaitLockRelease(ctx->lock);
    WdfRequestCompleteWithInformation(request, status, used);
}
static VOID Stop(WDFDEVICE device)
{
    NV_CONTEXT *ctx = NvContext(device);
    if (InterlockedExchange(&ctx->stopping, 1)) return;
    if (ctx->timer) WdfTimerStop(ctx->timer, TRUE);
    WdfWaitLockAcquire(ctx->lock, NULL);
    (void)Save(ctx, TRUE);
    ctx->query.stopped = 1;
    WdfWaitLockRelease(ctx->lock);
}
_Use_decl_annotations_
VOID NvShutdown(WDFDEVICE device) { Stop(device); }
_Use_decl_annotations_
VOID NvUnload(WDFDRIVER driver)
{
    UNREFERENCED_PARAMETER(driver);
    if (NvDevice) { Stop(NvDevice); WdfObjectDelete(NvDevice); NvDevice = NULL; }
}
_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDFDRIVER driver;
    PWDFDEVICE_INIT init;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_TIMER_CONFIG timer;
    WDF_IO_QUEUE_CONFIG queue;
    WDFDEVICE device;
    WDFMEMORY memory;
    NV_CONTEXT *ctx;
    DECLARE_CONST_UNICODE_STRING(security, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    DECLARE_CONST_UNICODE_STRING(name, L"\\Device\\Pi5Nvram");
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\Pi5Nvram");
    NTSTATUS status;
    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags = WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload = NvUnload;
    status = WdfDriverCreate(object, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) return status;
    init = WdfControlDeviceInitAllocate(driver, &security);
    if (!init) return STATUS_INSUFFICIENT_RESOURCES;
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    WdfControlDeviceInitSetShutdownNotification(init, NvShutdown, WdfDeviceShutdown);
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) { WdfDeviceInitFree(init); return status; }
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, NV_CONTEXT);
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    attr.SynchronizationScope = WdfSynchronizationScopeNone;
    status = WdfDeviceCreate(&init, &attr, &device);
    if (!NT_SUCCESS(status)) { if (init) WdfDeviceInitFree(init); return status; }
    ctx = NvContext(device);
    ctx->query.size = sizeof(ctx->query); ctx->query.version = NV_DRIVER_VERSION;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr); attr.ParentObject = device;
    status = WdfWaitLockCreate(&attr, &ctx->lock);
    if (NT_SUCCESS(status)) status = WdfMemoryCreate(&attr, NonPagedPoolNx, 'Nv5P', NV_FILE_SIZE,
                                                    &memory, (PVOID *)&ctx->snapshot);
    if (NT_SUCCESS(status)) status = WdfMemoryCreate(&attr, NonPagedPoolNx, 'Nv5P', 2 * NV_FILE_SIZE,
                                                    &memory, (PVOID *)&ctx->copies);
    if (!NT_SUCCESS(status)) goto Failed;
    WDF_TIMER_CONFIG_INIT(&timer, NvTimer); timer.AutomaticSerialization = FALSE;
    attr.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timer, &attr, &ctx->timer);
    if (!NT_SUCCESS(status)) goto Failed;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.PowerManaged = WdfFalse; queue.EvtIoDeviceControl = NvControl;
    status = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) goto Failed;
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) goto Failed;
    NvDevice = device;
    WdfControlFinishInitializing(device);
    WdfTimerStart(ctx->timer, WDF_REL_TIMEOUT_IN_MS(1000));
    return STATUS_SUCCESS;
Failed:
    WdfObjectDelete(device);
    return status;
}
