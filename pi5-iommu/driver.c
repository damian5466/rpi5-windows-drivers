/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include "pi5-iommu.h"
#include "hardware.h"

#define MMIO_BYTES 0x80u
#define GUARD_WORD 0xbadc0ffeu
typedef struct {
    volatile uint32_t *Cpu;
    PHYSICAL_ADDRESS Dma;
    ULONG Length, Pages, Writable, Mapped;
    ULONGLONG Token;
} BUFFER;
typedef struct {
    WDFDEVICE Device;
    WDFWAITLOCK Lock;
    PUCHAR Registers[4];
    PDMA_ADAPTER Adapter;
    WDFFILEOBJECT Owner;
    MMU_IO Io;
    MMU_HW Hw;
    BUFFER Root, L2, Trap, Buffers[MMU_SLOTS];
    PI5_IOMMU_STATUS Status;
    ULONG Executions;
} MMU_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MMU_CONTEXT, Context)
static volatile LONG BootFault;
static ULONGLONG NextToken;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Add;
EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE ReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY D0Entry;
EVT_WDF_DEVICE_D0_EXIT D0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Control;
EVT_WDF_DEVICE_QUERY_STOP QueryStop;
EVT_WDF_DEVICE_QUERY_REMOVE QueryRemove;
EVT_WDF_FILE_CLEANUP Cleanup;

static uint32_t Read(void *context, uint32_t unit, uint32_t offset)
{
    MMU_CONTEXT *c = context;
    return READ_REGISTER_ULONG((PULONG)(c->Registers[unit] + offset));
}
static void Write(void *context, uint32_t unit, uint32_t offset, uint32_t value)
{
    MMU_CONTEXT *c = context;
    WRITE_REGISTER_ULONG((PULONG)(c->Registers[unit] + offset), value);
}
static void Barrier(void *context)
{
    UNREFERENCED_PARAMETER(context);
    KeMemoryBarrier();
}
static void Delay(void *context)
{
    UNREFERENCED_PARAMETER(context);
    KeStallExecutionProcessor(1);
}

/* Volatile registry state survives driver unload/reload, but not a reboot.
 * Losing a devnode must never make uncertain DMA memory reusable. */
static NTSTATUS FaultMarker(BOOLEAN create)
{
    OBJECT_ATTRIBUTES attributes;
    HANDLE key;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, L"\\Registry\\Machine\\HARDWARE\\Pi5IommuFault");
    InitializeObjectAttributes(&attributes, (PUNICODE_STRING)&name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (create)
        status = ZwCreateKey(&key, KEY_READ, &attributes, 0, NULL, REG_OPTION_VOLATILE, NULL);
    else status = ZwOpenKey(&key, KEY_READ, &attributes);
    if (NT_SUCCESS(status)) ZwClose(key);
    return status;
}

static NTSTATUS Quarantine(MMU_CONTEXT *c, NTSTATUS status)
{
    InterlockedExchange(&BootFault, 1);
    c->Hw.Fault = 1;
    c->Status.Fault = 1;
    c->Status.LastStatus = (ULONG)status;
    ++c->Status.Failures;
    WdfDeviceSetStaticStopRemove(c->Device, FALSE);
    (void)FaultMarker(TRUE);
#if DBG
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "Pi5Iommu: quarantine status=%08lx writes=%lu flushes=%lu\n",
        status, c->Hw.Writes, c->Hw.Flushes);
#endif
    return status;
}

static NTSTATUS Result(MMU_CONTEXT *c, int result)
{
    if (result == MmuOk) return STATUS_SUCCESS;
    if (c->Hw.Fault) return Quarantine(c,
        result == MmuTimeout ? STATUS_IO_TIMEOUT : STATUS_DEVICE_HARDWARE_ERROR);
    return result == MmuBusy ? STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
}

static VOID FreeMemory(MMU_CONTEXT *c, BUFFER *b)
{
    if (b->Cpu) {
        c->Adapter->DmaOperations->FreeCommonBuffer(c->Adapter, b->Length,
            b->Dma, (PVOID)b->Cpu, FALSE);
        c->Status.AllocatedBytes -= b->Length;
        RtlZeroMemory(b, sizeof(*b));
    }
}

_Success_(return >= 0)
_At_(b->Cpu, _Post_notnull_)
static NTSTATUS AllocateMemory(MMU_CONTEXT *c, BUFFER *b, ULONG pages)
{
    PHYSICAL_ADDRESS maximum;
    ULONG i;
    maximum.QuadPart = MMU_DMA_LIMIT;
    b->Length = pages * MMU_PAGE;
    b->Cpu = c->Adapter->DmaOperations->AllocateCommonBufferEx(c->Adapter,
        &maximum, b->Length, &b->Dma, FALSE, 0);
    if (!b->Cpu) { b->Length = 0; return STATUS_INSUFFICIENT_RESOURCES; }
    c->Status.AllocatedBytes += b->Length;
    if (((ULONGLONG)b->Dma.QuadPart & (MMU_PAGE - 1u)) ||
        ((ULONG_PTR)b->Cpu & (MMU_PAGE - 1u)) ||
        (ULONGLONG)b->Dma.QuadPart > MMU_DMA_LIMIT ||
        b->Length - 1u > MMU_DMA_LIMIT - (ULONGLONG)b->Dma.QuadPart) {
        FreeMemory(c, b);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    /* _CCA=0 common buffers are uncached Device Memory. Never use a vector
     * memset/memcpy or unaligned access; barriers publish aligned stores. */
    for (i = 0; i < b->Length / sizeof(ULONG); ++i) b->Cpu[i] = 0;
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}

static NTSTATUS Begin(MMU_CONTEXT *c, WDFFILEOBJECT file)
{
    NTSTATUS status;
    if (BootFault) return STATUS_DEVICE_HARDWARE_ERROR;
    if (c->Owner) return STATUS_DEVICE_BUSY;
    status = AllocateMemory(c, &c->Root, 1);
    if (!NT_SUCCESS(status)) goto Fail;
    status = AllocateMemory(c, &c->L2, 1);
    if (!NT_SUCCESS(status)) goto Fail;
    status = AllocateMemory(c, &c->Trap, 1);
    if (!NT_SUCCESS(status)) goto Fail;
    c->Root.Cpu[0] = MMU_PTE_VALID | (ULONG)((ULONGLONG)c->L2.Dma.QuadPart >> 12);
    status = Result(c, MmuBegin(&c->Io, &c->Hw,
        (ULONGLONG)c->Root.Dma.QuadPart, (ULONGLONG)c->Trap.Dma.QuadPart));
    if (!NT_SUCCESS(status)) goto Fail;
    c->Owner = file;
    WdfDeviceSetStaticStopRemove(c->Device, FALSE);
    return STATUS_SUCCESS;
Fail:
    if (!c->Hw.Fault) {
        FreeMemory(c, &c->Root); FreeMemory(c, &c->L2); FreeMemory(c, &c->Trap);
    }
    return status;
}

static NTSTATUS Allocate(MMU_CONTEXT *c, const PI5_IOMMU_REQUEST *in,
                         PI5_IOMMU_BUFFER *out)
{
    ULONG slot, i;
    BUFFER *b;
    NTSTATUS status = Result(c, MmuCheck(&c->Io, &c->Hw));
    if (!NT_SUCCESS(status)) return status;
    for (slot = 0; slot < MMU_SLOTS && c->Buffers[slot].Cpu; ++slot) { }
    if (slot == MMU_SLOTS || NextToken == MAXULONGLONG) return STATUS_INSUFFICIENT_RESOURCES;
    b = &c->Buffers[slot];
    status = AllocateMemory(c, b, in->Pages + 2u);
    if (!NT_SUCCESS(status)) return status;
    b->Pages = in->Pages; b->Writable = in->Writable;
    for (i = 0; i < MMU_WORDS; ++i) {
        b->Cpu[i] = GUARD_WORD;
        b->Cpu[(b->Pages + 1) * MMU_WORDS + i] = GUARD_WORD;
    }
    status = Result(c, MmuMap(c->L2.Cpu, slot,
        (ULONGLONG)b->Dma.QuadPart + MMU_PAGE, b->Pages, b->Writable));
    if (!NT_SUCCESS(status)) { FreeMemory(c, b); return status; }
    b->Mapped = 1;
    /* Count before flushing: failure retains a fully accounted allocation. */
    b->Token = ++NextToken;
    ++c->Status.ActiveBuffers;
    ++c->Status.Allocations;
    status = Result(c, MmuInvalidate(&c->Io, &c->Hw));
    if (!NT_SUCCESS(status)) return status;
    RtlZeroMemory(out, sizeof(*out));
    out->Token = b->Token;
    out->Iova = MMU_IOVA_BASE + (ULONGLONG)(slot * MMU_SLOT_PAGES + 1) * MMU_PAGE;
    out->DmaAddress = (ULONGLONG)b->Dma.QuadPart + MMU_PAGE;
    out->CpuAddress = (ULONGLONG)(ULONG_PTR)(b->Cpu + MMU_WORDS);
    out->Bytes = b->Pages * MMU_PAGE;
    out->Writable = b->Writable;
    out->StagedOnly = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS Retire(MMU_CONTEXT *c, ULONGLONG token, BOOLEAN release)
{
    ULONG slot, i;
    BUFFER *b;
    NTSTATUS status;
    for (slot = 0; slot < MMU_SLOTS; ++slot)
        if (c->Buffers[slot].Cpu && c->Buffers[slot].Token == token) break;
    if (slot == MMU_SLOTS) return STATUS_NOT_FOUND;
    b = &c->Buffers[slot];
    status = Result(c, MmuCheck(&c->Io, &c->Hw));
    if (!NT_SUCCESS(status)) return status;
    KeMemoryBarrier();
    for (i = 0; i < MMU_WORDS; ++i)
        if (b->Cpu[i] != GUARD_WORD ||
            b->Cpu[(b->Pages + 1) * MMU_WORDS + i] != GUARD_WORD)
            return Quarantine(c, STATUS_DATA_ERROR);
    ++c->Status.GuardChecks;
    if (!b->Mapped && !release) return STATUS_ALREADY_COMPLETE;
    if (b->Mapped) {
        if (MmuUnmap(c->L2.Cpu, slot, b->Pages))
            return Quarantine(c, STATUS_DATA_ERROR);
        status = Result(c, MmuInvalidate(&c->Io, &c->Hw));
        if (!NT_SUCCESS(status)) return status;
        b->Mapped = 0;
    }
    if (!release) return STATUS_SUCCESS;
    FreeMemory(c, b);
    --c->Status.ActiveBuffers;
    ++c->Status.Frees;
    return STATUS_SUCCESS;
}

static NTSTATUS Free(MMU_CONTEXT *c, ULONGLONG token)
{
    return Retire(c, token, TRUE);
}

static NTSTATUS End(MMU_CONTEXT *c)
{
    NTSTATUS status;
    if (c->Status.ActiveBuffers) return STATUS_DEVICE_BUSY;
    status = Result(c, MmuEnd(&c->Io, &c->Hw));
    if (!NT_SUCCESS(status)) return status;
    FreeMemory(c, &c->Root); FreeMemory(c, &c->L2); FreeMemory(c, &c->Trap);
    c->Owner = NULL;
    WdfDeviceSetStaticStopRemove(c->Device, TRUE);
    return STATUS_SUCCESS;
}

static VOID Execute(MMU_CONTEXT *c, const PI5_IOMMU_EXECUTION *in,
                    PI5_IOMMU_EXECUTION_RESULT *out)
{
    NTSTATUS status, stopped;
    ULONG slot, i;
    int result;
    RtlZeroMemory(out, sizeof(*out));
    out->PrepareStatus = out->TransferStatus = out->QuiesceStatus = (ULONG)STATUS_NOT_SUPPORTED;
    status = Result(c, MmuCheck(&c->Io, &c->Hw));
    if (!NT_SUCCESS(status)) goto Done;
    if (!c->Status.ActiveBuffers) { status = STATUS_INVALID_DEVICE_STATE; goto Done; }
    status = in->Prepare(in->Context);
    out->PrepareStatus = (ULONG)status;
    if (status != STATUS_SUCCESS && NT_SUCCESS(status)) status = STATUS_DEVICE_PROTOCOL_ERROR;
    if (!NT_SUCCESS(status)) goto Done;
    ++c->Executions;
    status = Result(c, MmuActivate(&c->Io, &c->Hw));
    if (NT_SUCCESS(status)) {
        status = in->Transfer(in->Context);
        out->TransferStatus = (ULONG)status;
        if (status != STATUS_SUCCESS && NT_SUCCESS(status)) status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    /* An activation may have partially succeeded. Always ask the owner to
     * drain and remove its IOVAs; never infer a stop from Transfer's status. */
    stopped = in->Quiesce(in->Context);
    out->QuiesceStatus = (ULONG)stopped;
    if (stopped != STATUS_SUCCESS && NT_SUCCESS(stopped)) stopped = STATUS_DEVICE_PROTOCOL_ERROR;
    result = MmuDeactivate(&c->Io, &c->Hw, NT_SUCCESS(stopped));
    if (result != MmuOk) status = Result(c, result);
    if (!NT_SUCCESS(stopped)) status = Quarantine(c, stopped);
    if (!c->Hw.Active && NT_SUCCESS(stopped)) {
        KeMemoryBarrier();
        for (slot = 0; slot < MMU_SLOTS; ++slot) {
            BUFFER *b = &c->Buffers[slot];
            if (!b->Cpu) continue;
            for (i = 0; i < MMU_WORDS; ++i)
                if (b->Cpu[i] != GUARD_WORD ||
                    b->Cpu[(b->Pages + 1) * MMU_WORDS + i] != GUARD_WORD) {
                    status = Quarantine(c, STATUS_DATA_ERROR);
                    goto Done;
                }
            ++c->Status.GuardChecks;
        }
        for (i = 0; i < MMU_WORDS; ++i)
            if (c->Trap.Cpu[i]) ++out->TrapWordsChanged;
    }
Done:
    out->Status = (ULONG)status;
    out->Active = c->Hw.Active;
    out->FaultFlags = c->Hw.FaultFlags;
    out->ViolationAddress = c->Hw.ViolationAddress;
    out->Hits = c->Hw.Hits; out->Misses = c->Hw.Misses; out->Stalls = c->Hw.Stalls;
    out->Executions = c->Executions;
    if (!BootFault) c->Status.LastStatus = (ULONG)status;
}

static VOID Snapshot(MMU_CONTEXT *c)
{
    static const ULONG offsets[] = {0,4,0x14,0x1c,0x20,0x24,0x30,0x34,0x38};
    ULONG i, j;
    for (i = 0; i < 3; ++i) {
        ULONG *values = (ULONG *)&c->Status.Units[i];
        for (j = 0; j < RTL_NUMBER_OF(offsets); ++j)
            values[j] = READ_REGISTER_ULONG((PULONG)(c->Registers[i] + offsets[j]));
    }
    c->Status.CacheControl = READ_REGISTER_ULONG((PULONG)c->Registers[3]);
    c->Status.HardwareWrites = c->Hw.Writes;
    c->Status.Flushes = c->Hw.Flushes;
    c->Status.Owner = c->Owner != NULL;
    c->Status.Staged = c->Hw.Staged;
    c->Status.Fault = BootFault != 0;
}

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, Add);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS Add(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_FILEOBJECT_CONFIG files;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, PI5_IOMMU_NAME);
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\Pi5Iommu");
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GR;;;BA)");
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, TRUE);
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(init, &sddl);
    if (!NT_SUCCESS(status)) return status;
    WDF_FILEOBJECT_CONFIG_INIT(&files, WDF_NO_EVENT_CALLBACK,
        WDF_NO_EVENT_CALLBACK, Cleanup);
    WdfDeviceInitSetFileObjectConfig(init, &files, WDF_NO_OBJECT_ATTRIBUTES);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = Prepare;
    pnp.EvtDeviceReleaseHardware = ReleaseHardware;
    pnp.EvtDeviceD0Entry = D0Entry;
    pnp.EvtDeviceD0Exit = D0Exit;
    pnp.EvtDeviceQueryStop = QueryStop;
    pnp.EvtDeviceQueryRemove = QueryRemove;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MMU_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    Context(device)->Device = device;
    Context(device)->Io.Context = Context(device);
    Context(device)->Io.Read = Read;
    Context(device)->Io.Write = Write;
    Context(device)->Io.Barrier = Barrier;
    Context(device)->Io.Delay = Delay;
    Context(device)->Status.Version = PI5_IOMMU_VERSION;
    Context(device)->Status.Debug = DBG;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
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
    static const ULONGLONG addresses[] = {
        0x1000005100ULL, 0x1000005200ULL, 0x1000005280ULL, 0x1000005b00ULL};
    MMU_CONTEXT *c = Context(device);
    ULONG i, j, found = 0;
    ULONG maps = 0;
    DEVICE_DESCRIPTION description = {0};
    NTSTATUS status;
    UNREFERENCED_PARAMETER(raw);
    if (BootFault) return STATUS_DEVICE_HARDWARE_ERROR;
    status = FaultMarker(FALSE);
    if (NT_SUCCESS(status)) { InterlockedExchange(&BootFault, 1); return STATUS_DEVICE_HARDWARE_ERROR; }
    if (status != STATUS_OBJECT_NAME_NOT_FOUND && status != STATUS_OBJECT_PATH_NOT_FOUND)
        return status;
    for (i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeNull || r->Type == CmResourceTypeDevicePrivate) continue;
        if (r->Type != CmResourceTypeMemory || r->u.Memory.Length != MMIO_BYTES)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        for (j = 0; j < RTL_NUMBER_OF(addresses); ++j)
            if ((ULONGLONG)r->u.Memory.Start.QuadPart == addresses[j]) break;
        if (j == RTL_NUMBER_OF(addresses) || (found & (1u << j)))
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        c->Registers[j] = MmMapIoSpaceEx(r->u.Memory.Start, MMIO_BYTES,
            (j == 1 || j == 3 ? PAGE_READWRITE : PAGE_READONLY) | PAGE_NOCACHE);
        if (!c->Registers[j]) return STATUS_INSUFFICIENT_RESOURCES;
        found |= 1u << j;
    }
    if (found != 15) return STATUS_DEVICE_CONFIGURATION_ERROR;
    Snapshot(c);
    description.Version = DEVICE_DESCRIPTION_VERSION3;
    description.Master = TRUE; description.ScatterGather = TRUE;
    description.InterfaceType = Internal; description.DmaAddressWidth = 36;
    description.MaximumLength = (MMU_MAX_PAGES + 2u) * MMU_PAGE;
    c->Adapter = IoGetDmaAdapter(WdfDeviceWdmGetPhysicalDevice(device), &description, &maps);
    if (!c->Adapter) return STATUS_INSUFFICIENT_RESOURCES;
    if (c->Adapter->DmaOperations->Size < FIELD_OFFSET(DMA_OPERATIONS, AllocateCommonBufferEx) +
        sizeof(c->Adapter->DmaOperations->AllocateCommonBufferEx) ||
        !c->Adapter->DmaOperations->AllocateCommonBufferEx) return STATUS_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS ReleaseHardware(WDFDEVICE device, WDFCMRESLIST translated)
{
    MMU_CONTEXT *c = Context(device);
    ULONG i;
    UNREFERENCED_PARAMETER(translated);
    WdfWaitLockAcquire(c->Lock, NULL);
    if (c->Owner || c->Hw.Staged) (void)Quarantine(c, STATUS_DEVICE_BUSY);
    c->Status.Online = 0;
    for (i = 0; i < 4; ++i) {
        if (c->Registers[i]) MmUnmapIoSpace(c->Registers[i], MMIO_BYTES);
        c->Registers[i] = NULL;
    }
    if (c->Adapter && !BootFault) {
        c->Adapter->DmaOperations->PutDmaAdapter(c->Adapter);
        c->Adapter = NULL;
    }
    /* On uncertainty, adapter and common buffers intentionally survive this
     * devnode, and the volatile marker refuses another owner until reboot. */
    WdfWaitLockRelease(c->Lock);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS D0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    MMU_CONTEXT *c = Context(device);
    UNREFERENCED_PARAMETER(previous);
    WdfWaitLockAcquire(c->Lock, NULL);
    c->Status.Online = !BootFault;
    WdfWaitLockRelease(c->Lock);
    return BootFault ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS D0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    MMU_CONTEXT *c = Context(device);
    UNREFERENCED_PARAMETER(target);
    WdfWaitLockAcquire(c->Lock, NULL);
    c->Status.Online = 0;
    if (c->Owner || c->Hw.Staged) (void)Quarantine(c, STATUS_DEVICE_BUSY);
    WdfWaitLockRelease(c->Lock);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS QueryStop(WDFDEVICE device)
{
    MMU_CONTEXT *c = Context(device);
    NTSTATUS status;
    WdfWaitLockAcquire(c->Lock, NULL);
    status = c->Owner || BootFault ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    WdfWaitLockRelease(c->Lock);
    return status;
}
_Use_decl_annotations_
NTSTATUS QueryRemove(WDFDEVICE device) { return QueryStop(device); }

_Use_decl_annotations_
VOID Cleanup(WDFFILEOBJECT file)
{
    MMU_CONTEXT *c = Context(WdfFileObjectGetDevice(file));
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;
    WdfWaitLockAcquire(c->Lock, NULL);
    if (c->Owner == file) {
        ++c->Status.Cleanups;
        for (i = 0; i < MMU_SLOTS && !BootFault; ++i)
            if (c->Buffers[i].Cpu && !NT_SUCCESS(Free(c, c->Buffers[i].Token))) break;
        if (!BootFault) status = End(c);
        if (!NT_SUCCESS(status) && !BootFault) (void)Quarantine(c, status);
        c->Owner = NULL;
    }
    WdfWaitLockRelease(c->Lock);
}

static BOOLEAN ValidRequest(const PI5_IOMMU_REQUEST *in, ULONG code)
{
    if (in->Version != PI5_IOMMU_VERSION || in->Unit != 4) return FALSE;
    if (code == IOCTL_PI5_IOMMU_ALLOCATE)
        return in->Pages >= 1 && in->Pages <= MMU_MAX_PAGES &&
            in->Writable <= 1 && !in->Token && !in->Address;
    if (code == IOCTL_PI5_IOMMU_FREE || code == IOCTL_PI5_IOMMU_RETIRE)
        return !in->Pages && !in->Writable && in->Token && !in->Address;
    if (code == IOCTL_PI5_IOMMU_WALK)
        return !in->Pages && in->Writable <= 1 && !in->Token;
    return !in->Pages && !in->Writable && !in->Token && !in->Address;
}

_Use_decl_annotations_
VOID Control(WDFQUEUE queue, WDFREQUEST request, size_t outputLength,
             size_t inputLength, ULONG code)
{
    MMU_CONTEXT *c = Context(WdfIoQueueGetDevice(queue));
    PI5_IOMMU_STATUS *out;
    PI5_IOMMU_REQUEST *in, copy = {0};
    PI5_IOMMU_BUFFER *buffer = NULL;
    PI5_IOMMU_EXECUTION *executionIn, execution = {0};
    PI5_IOMMU_EXECUTION_RESULT *executionOut = NULL;
    WDFFILEOBJECT file = WdfRequestGetFileObject(request);
    NTSTATUS status;
    size_t written = 0;
    if (code != IOCTL_PI5_IOMMU_QUERY && code != IOCTL_PI5_IOMMU_BEGIN &&
        code != IOCTL_PI5_IOMMU_ALLOCATE && code != IOCTL_PI5_IOMMU_FREE &&
        code != IOCTL_PI5_IOMMU_SYNC && code != IOCTL_PI5_IOMMU_END &&
        code != IOCTL_PI5_IOMMU_WALK && code != IOCTL_PI5_IOMMU_EXECUTE &&
        code != IOCTL_PI5_IOMMU_RETIRE) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    if (code == IOCTL_PI5_IOMMU_QUERY) {
        if (inputLength) { WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return; }
    } else if (code == IOCTL_PI5_IOMMU_EXECUTE) {
        if (WdfRequestGetRequestorMode(request) != KernelMode || !file) {
            WdfRequestComplete(request, STATUS_ACCESS_DENIED); return;
        }
        if (inputLength != sizeof(execution) || outputLength != sizeof(*executionOut)) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
        }
        status = WdfRequestRetrieveInputBuffer(request, sizeof(execution), (PVOID *)&executionIn, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        execution = *executionIn;
        if (execution.Version != PI5_IOMMU_VERSION || execution.Unit != 4 ||
            !execution.Prepare || !execution.Transfer || !execution.Quiesce) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
        }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*executionOut), (PVOID *)&executionOut, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    } else {
        if (WdfRequestGetRequestorMode(request) != KernelMode || !file) {
            WdfRequestComplete(request, STATUS_ACCESS_DENIED); return;
        }
        if (inputLength != sizeof(copy) ||
            ((code == IOCTL_PI5_IOMMU_ALLOCATE || code == IOCTL_PI5_IOMMU_WALK) ?
                outputLength != sizeof(*buffer) : outputLength != 0)) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return;
        }
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*in), (PVOID *)&in, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        copy = *in;
        if (!ValidRequest(&copy, code)) { WdfRequestComplete(request, STATUS_INVALID_PARAMETER); return; }
        if (outputLength) {
            status = WdfRequestRetrieveOutputBuffer(request, sizeof(*buffer), (PVOID *)&buffer, NULL);
            if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
        }
    }
    WdfWaitLockAcquire(c->Lock, NULL);
    status = STATUS_DEVICE_NOT_READY;
    if (c->Status.Online && code == IOCTL_PI5_IOMMU_QUERY) {
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), (PVOID *)&out, NULL);
        if (NT_SUCCESS(status)) {
            Snapshot(c);
            *out = c->Status;
            written = sizeof(*out);
        }
    } else if (c->Status.Online) {
        if (BootFault) status = STATUS_DEVICE_HARDWARE_ERROR;
        else if (code == IOCTL_PI5_IOMMU_BEGIN) status = Begin(c, file);
        else if (!c->Owner || c->Owner != file) status = STATUS_ACCESS_DENIED;
        else if (code == IOCTL_PI5_IOMMU_EXECUTE) {
            Execute(c, &execution, executionOut);
            written = sizeof(*executionOut);
            status = STATUS_SUCCESS;
        }
        else if (code == IOCTL_PI5_IOMMU_ALLOCATE) status = Allocate(c, &copy, buffer);
        else if (code == IOCTL_PI5_IOMMU_FREE) status = Free(c, copy.Token);
        else if (code == IOCTL_PI5_IOMMU_RETIRE) status = Retire(c, copy.Token, FALSE);
        else if (code == IOCTL_PI5_IOMMU_SYNC) status = Result(c, MmuInvalidate(&c->Io, &c->Hw));
        else if (code == IOCTL_PI5_IOMMU_END) status = End(c);
        else {
            status = Result(c, MmuCheck(&c->Io, &c->Hw));
            if (NT_SUCCESS(status)) {
                RtlZeroMemory(buffer, sizeof(*buffer));
                status = Result(c, MmuWalk(c->L2.Cpu, copy.Address,
                    copy.Writable, &buffer->DmaAddress));
                buffer->StagedOnly = 1;
            }
        }
        if (NT_SUCCESS(status) && buffer) written = sizeof(*buffer);
        if (!BootFault && code != IOCTL_PI5_IOMMU_EXECUTE) c->Status.LastStatus = (ULONG)status;
    }
    WdfWaitLockRelease(c->Lock);
    WdfRequestCompleteWithInformation(request, status, written);
}
