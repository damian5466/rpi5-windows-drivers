/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include "pi5-v3d.h"
#include "pi5-fclk.h"
#include "pi5-pm.h"
#include "pi5-graph.h"
#include "hardware.h"
#include "shader.h"

#define GUARD 0xbadc0ffeu
typedef struct {
    volatile uint32_t *Cpu;
    PHYSICAL_ADDRESS Dma;
    ULONG Bytes;
} V3D_BUFFER;
typedef struct {
    V3D_BUFFER Buffer;
    ULONG Handle, Address, Bytes, Flags;
} V3D_OBJECT;
typedef struct {
    WDFDEVICE Device;
    WDFWAITLOCK Lock;
    WDFIOTARGET Clock, Pm;
    WDFINTERRUPT Interrupt[2];
    volatile LONG IrqLive[2], Pending[2], InterruptCount[2];
    KEVENT Wake;
    PUCHAR Registers[3];
    V3D_IO Io;
    PDMA_ADAPTER Adapter;
    V3D_BUFFER Table, Trap, Source, Destination, Code, Uniform, Tile, TileState;
    V3D_OBJECT Objects[PI5_V3D_BUFFER_LIMIT];
    ULONG ObjectSerial, ObjectBytes;
    ULONGLONG MemorySubmitted, MemoryCompleted;
    BOOLEAN MemorySession;
    WDFFILEOBJECT Owner;
    BOOLEAN ClockLease, PmLease, Powered, Claimed, Woke;
    PI5_V3D_STATUS Status;
    PI5_V3D_COMPUTE_STATUS ComputeStatus;
    PI5_V3D_RENDER_STATUS RenderStatus;
} V3D_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(V3D_CONTEXT, Context)
static volatile LONG BootFault;
static const ULONGLONG Bases[] = {0x1002000000ull, 0x1002008000ull, 0x1002030800ull};
static const ULONG Lengths[] = {0x4000, 0x6000, 0x700};

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Add;
EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE ReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY Entry;
EVT_WDF_DEVICE_D0_EXIT Exit;
EVT_WDF_DEVICE_QUERY_STOP QueryStop;
EVT_WDF_DEVICE_QUERY_REMOVE QueryRemove;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Control;
EVT_WDF_FILE_CLEANUP Cleanup;
EVT_WDF_INTERRUPT_ISR Isr;
EVT_WDF_INTERRUPT_DPC Dpc;
EVT_WDF_INTERRUPT_ENABLE InterruptEnable;
EVT_WDF_INTERRUPT_DISABLE InterruptDisable;
EVT_WDF_IO_TARGET_QUERY_REMOVE TargetQueryRemove;

static uint32_t Read(void *context, uint32_t unit, uint32_t offset)
{
    V3D_CONTEXT *c = context;
    return READ_REGISTER_ULONG((PULONG)(c->Registers[unit] + offset));
}
static void Write(void *context, uint32_t unit, uint32_t offset, uint32_t value)
{
    V3D_CONTEXT *c = context;
    WRITE_REGISTER_ULONG((PULONG)(c->Registers[unit] + offset), value);
}
static void Delay(void *context)
{
    UNREFERENCED_PARAMETER(context);
    KeStallExecutionProcessor(10);
}
static NTSTATUS Hw(int result)
{
    return result == V3dOk ? STATUS_SUCCESS : result == V3dTimeout ? STATUS_IO_TIMEOUT :
        result == V3dBusy ? STATUS_DEVICE_BUSY : STATUS_INVALID_PARAMETER;
}
static NTSTATUS Marker(BOOLEAN create)
{
    OBJECT_ATTRIBUTES a;
    HANDLE key;
    NTSTATUS s;
    DECLARE_CONST_UNICODE_STRING(name, L"\\Registry\\Machine\\HARDWARE\\Pi5V3dFault");
    InitializeObjectAttributes(&a, (PUNICODE_STRING)&name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    s = create ? ZwCreateKey(&key, KEY_READ, &a, 0, NULL, REG_OPTION_VOLATILE, NULL) :
        ZwOpenKey(&key, KEY_READ, &a);
    if (NT_SUCCESS(s)) ZwClose(key);
    return s;
}
static NTSTATUS Fault(V3D_CONTEXT *c, NTSTATUS s)
{
    InterlockedExchange(&BootFault, 1);
    c->Status.Fault = 1;
    c->Status.LastStatus = (ULONG)s;
    ++c->Status.Failures;
    WdfDeviceSetStaticStopRemove(c->Device, FALSE);
    (void)Marker(TRUE);
#if DBG
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
        "Pi5V3d: contained failure phase=%lu status=%08lx\n", c->Status.Phase, s);
#endif
    return s;
}
static VOID Snapshot(V3D_CONTEXT *c)
{
    PI5_V3D_STATUS *s = &c->Status;
    ULONG i;
    s->Owner = c->Owner != NULL;
    s->Fault = BootFault != 0;
    s->HubInterrupts = (ULONG)c->InterruptCount[0];
    s->CoreInterrupts = (ULONG)c->InterruptCount[1];
    s->HubPending = (ULONG)c->Pending[0]; s->CorePending = (ULONG)c->Pending[1];
    if (!c->Powered) return;
    s->SmsRee = Read(c, V3dSms, 0); s->SmsTee = Read(c, V3dSms, 0x400);
    if ((s->SmsTee & 15) != 0) return;
    for (i = 0; i < 4; ++i) s->HubIdent[i] = Read(c, V3dHub, 8 + i * 4);
    for (i = 0; i < 3; ++i) s->CoreIdent[i] = Read(c, V3dCore, i * 4);
    s->MmuInfo = Read(c, V3dHub, 0x1238);
    s->TechVersion = (s->HubIdent[1] & 15) * 10 + ((s->HubIdent[1] >> 4) & 15);
    c->ComputeStatus.IpRevision = (s->HubIdent[3] >> 8) & 255;
    s->PaBits = 30 + ((s->MmuInfo >> 8) & 15);
    s->VaBits = 30 + ((s->MmuInfo >> 4) & 15);
    s->Tfu = Read(c, V3dHub, V3D_TFU_CS); s->Csd = Read(c, V3dCore, 0x900);
    s->Ct0 = Read(c, V3dCore, 0x100); s->Ct1 = Read(c, V3dCore, 0x104);
    s->Gmp = Read(c, V3dHub, V3D_GMP_STATUS);
    s->MmuControl = Read(c, V3dHub, V3D_MMU_CTL); s->MmuCache = Read(c, V3dHub, V3D_MMUC);
    s->MmuVioId = Read(c, V3dHub, 0x122c); s->MmuVioAddress = Read(c, V3dHub, 0x1234);
    s->MmuHits = Read(c, V3dHub, 0x1208); s->MmuMisses = Read(c, V3dHub, 0x120c);
}

_Use_decl_annotations_
NTSTATUS TargetQueryRemove(WDFIOTARGET target)
{
    UNREFERENCED_PARAMETER(target);
    return STATUS_DEVICE_BUSY; /* Never abandon a clock or reset lease. */
}
static NTSTATUS Open(V3D_CONTEXT *c, PCWSTR path, ACCESS_MASK access, WDFIOTARGET *target)
{
    WDF_OBJECT_ATTRIBUTES a;
    WDF_IO_TARGET_OPEN_PARAMS p;
    UNICODE_STRING name;
    NTSTATUS s;
    if (*target) return STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = c->Device;
    s = WdfIoTargetCreate(c->Device, &a, target);
    if (!NT_SUCCESS(s)) return s;
    RtlInitUnicodeString(&name, path);
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&p, &name, access);
    p.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    p.EvtIoTargetQueryRemove = TargetQueryRemove;
    s = WdfIoTargetOpen(*target, &p);
    if (!NT_SUCCESS(s)) { WdfObjectDelete(*target); *target = NULL; }
    return s;
}
static VOID Close(WDFIOTARGET *target)
{
    if (*target) { WdfIoTargetClose(*target); WdfObjectDelete(*target); *target = NULL; }
}
static NTSTATUS Send(WDFIOTARGET target, ULONG code, void *in, ULONG il, void *out, ULONG ol)
{
    WDF_MEMORY_DESCRIPTOR a, b;
    ULONG_PTR used = 0;
    NTSTATUS s;
    if (in) WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&a, in, il);
    if (out) WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&b, out, ol);
    s = WdfIoTargetSendIoctlSynchronously(target, NULL, code,
        in ? &a : NULL, out ? &b : NULL, NULL, &used);
    if (s == STATUS_SUCCESS && used != ol) return STATUS_DEVICE_PROTOCOL_ERROR;
    return s;
}
static NTSTATUS Clock(V3D_CONTEXT *c, ULONG code, ULONG value)
{
    PI5_FCLK_REQUEST in = {1, PI5_FCLK_V3D, value, 0};
    PI5_FCLK_STATUS out;
    NTSTATUS s = Send(c->Clock, code, &in, sizeof(in), &out, sizeof(out));
    if (s == STATUS_SUCCESS && (out.Version != 1 || out.Id != 5 ||
        (out.Flags & PI5_FCLK_FLAG_UNCERTAIN))) return STATUS_DEVICE_PROTOCOL_ERROR;
    return s;
}
static NTSTATUS Pm(V3D_CONTEXT *c, ULONG code)
{
    PI5_PM_LEASE_REQUEST in = {1, 1, 0, 0};
    return Send(c->Pm, code, &in, sizeof(in), NULL, 0);
}
static NTSTATUS Pulse(V3D_CONTEXT *c)
{
    PI5_PM_TRANSITION in = {1, 1, Pi5PmV3dPulseReset, 0};
    return Send(c->Pm, IOCTL_PI5_PM_TRANSITION, &in, sizeof(in), NULL, 0);
}

static NTSTATUS Graph(V3D_CONTEXT *c)
{
    typedef struct { PI5_GRAPH_FIND Find; PI5_GRAPH_NODE Node; PI5_GRAPH_RESOLVE Resolve; } WORK;
    static const char *properties[] = {"clocks", "power-domains", "resets"};
    static const char *cells[] = {"#clock-cells", "#power-domain-cells", "#reset-cells"};
    static const char *owners[] = {"\\_SB.FCLK", "\\_SB.PM00", "\\_SB.PM00"};
    static const ULONG values[] = {5, 1, 0};
    WDFIOTARGET target = NULL;
    WORK *w;
    PI5_GRAPH_REFERENCE ref;
    PI5_GRAPH_KEY key;
    NTSTATUS s;
    ULONG i, gpu;
    w = ExAllocatePool2(POOL_FLAG_PAGED, sizeof(*w), 'g3VP');
    if (!w) return STATUS_INSUFFICIENT_RESOURCES;
    s = Open(c, PI5_GRAPH_NAME, GENERIC_READ, &target);
    if (s != STATUS_SUCCESS) goto Done;
    w->Find.Version = 1; w->Find.Kind = PI5_GRAPH_FIND_PATH;
    RtlCopyMemory(w->Find.Text, "/axi/v3d@2000000", sizeof("/axi/v3d@2000000"));
    s = Send(target, IOCTL_PI5_GRAPH_FIND, &w->Find, sizeof(w->Find), &w->Node, sizeof(w->Node));
    if (s != STATUS_SUCCESS) goto Done;
    if (strcmp(w->Node.Owner, "\\_SB.GPU0")) { s = STATUS_DEVICE_CONFIGURATION_ERROR; goto Done; }
    gpu = w->Node.Index;
    for (i = 0; i < 3; ++i) {
        RtlZeroMemory(&w->Resolve, sizeof(w->Resolve));
        w->Resolve.Version = 1; w->Resolve.Node = gpu;
        RtlCopyMemory(w->Resolve.Property, properties[i], strlen(properties[i]) + 1);
        RtlCopyMemory(w->Resolve.CellsProperty, cells[i], strlen(cells[i]) + 1);
        s = Send(target, IOCTL_PI5_GRAPH_RESOLVE, &w->Resolve, sizeof(w->Resolve), &ref, sizeof(ref));
        if (s != STATUS_SUCCESS) goto Done;
        if (ref.Version != 1 || ref.Count != 1 || ref.Cells[0] != values[i]) {
            s = STATUS_DEVICE_CONFIGURATION_ERROR; goto Done;
        }
        RtlZeroMemory(&key, sizeof(key)); key.Version = 1; key.Node = ref.Provider;
        s = Send(target, IOCTL_PI5_GRAPH_NODE, &key, sizeof(key), &w->Node, sizeof(w->Node));
        if (s != STATUS_SUCCESS) goto Done;
        if (strcmp(w->Node.Owner, owners[i])) { s = STATUS_DEVICE_CONFIGURATION_ERROR; goto Done; }
    }
Done:
    Close(&target); ExFreePool(w); return s;
}

static VOID FreeBuffer(V3D_CONTEXT *c, V3D_BUFFER *b)
{
    if (b->Cpu) {
        c->Adapter->DmaOperations->FreeCommonBuffer(c->Adapter, b->Bytes, b->Dma, (PVOID)b->Cpu, FALSE);
        c->Status.AllocatedBytes -= b->Bytes; RtlZeroMemory(b, sizeof(*b));
    }
}
static NTSTATUS Allocate(V3D_CONTEXT *c, V3D_BUFFER *b, ULONG bytes)
{
    PHYSICAL_ADDRESS maximum;
    ULONG i;
    maximum.QuadPart = (1ull << 36) - 1;
    b->Bytes = bytes + 2 * V3D_PAGE;
    b->Cpu = c->Adapter->DmaOperations->AllocateCommonBufferEx(c->Adapter,
        &maximum, b->Bytes, &b->Dma, FALSE, 0);
    if (!b->Cpu) { b->Bytes = 0; return STATUS_INSUFFICIENT_RESOURCES; }
    c->Status.AllocatedBytes += b->Bytes;
    if (((ULONGLONG)b->Dma.QuadPart & 4095) || ((ULONG_PTR)b->Cpu & 4095) ||
        (ULONGLONG)b->Dma.QuadPart > (1ull << 36) - b->Bytes) {
        FreeBuffer(c, b); return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    /* _CCA=0 HAL common buffers have no cached alias. Use aligned scalar
     * volatile accesses, never vector memset/memcpy or raw cache opcodes. */
    for (i = 0; i < b->Bytes / 4; ++i)
        b->Cpu[i] = i < 1024 || i >= (b->Bytes - V3D_PAGE) / 4 ? GUARD : 0;
    KeMemoryBarrier(); return STATUS_SUCCESS;
}
static NTSTATUS Guards(V3D_CONTEXT *c)
{
    V3D_BUFFER *buffers[] = {&c->Table, &c->Trap, &c->Source, &c->Destination,
        &c->Code, &c->Uniform, &c->Tile, &c->TileState};
    ULONG i, j;
    for (i = 0; i < RTL_NUMBER_OF(buffers); ++i) {
        V3D_BUFFER *b = buffers[i];
        if (!b->Cpu) continue;
        for (j = 0; j < 1024; ++j)
            if (b->Cpu[j] != GUARD || b->Cpu[(b->Bytes - V3D_PAGE) / 4 + j] != GUARD)
                ++c->Status.GuardFailures;
    }
    if (c->Trap.Cpu)
        for (j = 1024; j < 2048; ++j) if (c->Trap.Cpu[j]) ++c->Status.ScratchWrites;
    for (i = 0; i < PI5_V3D_BUFFER_LIMIT; ++i) {
        V3D_BUFFER *b = &c->Objects[i].Buffer;
        if (!b->Cpu) continue;
        for (j = 0; j < 1024; ++j)
            if (b->Cpu[j] != GUARD || b->Cpu[(b->Bytes - V3D_PAGE) / 4 + j] != GUARD)
                ++c->Status.GuardFailures;
    }
    return c->Status.GuardFailures || c->Status.ScratchWrites ? STATUS_DATA_ERROR : STATUS_SUCCESS;
}
static VOID Mask(V3D_CONTEXT *c, BOOLEAN enable)
{
    ULONG i;
    for (i = 0; i < 2; ++i) {
        WdfInterruptAcquireLock(c->Interrupt[i]);
        Write(c, i, V3D_INT_MASK_SET, UINT32_MAX);
        c->IrqLive[i] = enable;
        if (enable) {
            Write(c, i, V3D_INT_CLR, UINT32_MAX);
            Write(c, i, V3D_INT_MASK_CLR, i ? V3D_CORE_IRQS : V3D_HUB_IRQS);
        }
        WdfInterruptReleaseLock(c->Interrupt[i]);
    }
}
static NTSTATUS Reset(V3D_CONTEXT *c)
{
    NTSTATUS s;
    c->Status.Phase = 10;
    Mask(c, FALSE);
    s = Hw(V3dDrain(&c->Io));
    if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Hw(V3dSmsReset(&c->Io));
    if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Pulse(c);
    if (s != STATUS_SUCCESS) return Fault(c, s);
    c->Io.CsdIdle = 0; c->Io.RenderStart = 0; c->Io.RenderEnd = 0;
    c->Io.BinStart = 0; c->Io.BinEnd = 0;
    if (V3dIdle(&c->Io) || Read(c, V3dHub, V3D_MMU_CTL) & 1) return Fault(c, STATUS_DEVICE_HARDWARE_ERROR);
    Write(c, V3dCore, 0x34, 0); Write(c, V3dCore, 0x38, UINT32_MAX);
    if (c->Table.Cpu) {
        KeMemoryBarrier();
        s = Hw(V3dMmuStart(&c->Io, c->Status.TableDma, (ULONGLONG)c->Trap.Dma.QuadPart + V3D_PAGE));
        if (s != STATUS_SUCCESS) return Fault(c, s);
    }
    InterlockedExchange(&c->Pending[0], 0); InterlockedExchange(&c->Pending[1], 0);
    Mask(c, TRUE); ++c->Status.Resets; return STATUS_SUCCESS;
}
static NTSTATUS DropProviders(V3D_CONTEXT *c)
{
    NTSTATUS s;
    if (c->PmLease) {
        s = Pm(c, IOCTL_PI5_PM_RELEASE); if (s != STATUS_SUCCESS) return Fault(c, s);
        c->PmLease = FALSE;
    }
    if (c->ClockLease) {
        s = Clock(c, IOCTL_PI5_FCLK_RELEASE, 0); if (s != STATUS_SUCCESS) return Fault(c, s);
        c->ClockLease = FALSE;
    }
    Close(&c->Pm); Close(&c->Clock); c->Owner = NULL;
    WdfDeviceSetStaticStopRemove(c->Device, TRUE);
    return STATUS_SUCCESS;
}
static NTSTATUS End(V3D_CONTEXT *c)
{
    NTSTATUS s;
    ULONG i;
    if (BootFault) return STATUS_DEVICE_HARDWARE_ERROR;
    c->Status.Phase = 20;
    if (c->Claimed) {
        Mask(c, FALSE);
        s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
        s = Hw(V3dSmsReset(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
        s = Pulse(c); if (s != STATUS_SUCCESS) return Fault(c, s);
        c->Io.CsdIdle = 0; c->Io.RenderStart = 0; c->Io.RenderEnd = 0;
        c->Io.BinStart = 0; c->Io.BinEnd = 0;
        if (V3dIdle(&c->Io) || Read(c, V3dHub, V3D_MMU_CTL) & 1 ||
            Read(c, V3dHub, V3D_MMU_PT) || Read(c, V3dHub, V3D_MMU_TRAP))
            return Fault(c, STATUS_DEVICE_HARDWARE_ERROR);
        Mask(c, FALSE);
        s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
        c->Claimed = FALSE;
    }
    Snapshot(c);
    if (c->Woke) {
        Write(c, V3dSms, 0x400, 1u << 30);
        s = Hw(V3dPoll(&c->Io, V3dSms, 0x400, 15, 13));
        if (s != STATUS_SUCCESS) return Fault(c, s);
        c->Woke = FALSE;
    }
    Snapshot(c); c->Powered = FALSE;
    for (i = 0; i < PI5_V3D_BUFFER_LIMIT; ++i) {
        FreeBuffer(c, &c->Objects[i].Buffer);
        RtlZeroMemory(&c->Objects[i], sizeof(c->Objects[i]));
    }
    c->ObjectBytes = 0; c->MemorySession = FALSE;
    FreeBuffer(c, &c->TileState); FreeBuffer(c, &c->Tile);
    FreeBuffer(c, &c->Uniform); FreeBuffer(c, &c->Code);
    FreeBuffer(c, &c->Destination); FreeBuffer(c, &c->Source);
    FreeBuffer(c, &c->Trap); FreeBuffer(c, &c->Table);
    return DropProviders(c);
}
static NTSTATUS Begin(V3D_CONTEXT *c, WDFFILEOBJECT file)
{
    NTSTATUS s, clean;
    ULONG sms, i;
    if (BootFault) return STATUS_DEVICE_HARDWARE_ERROR;
    if (c->Owner) return STATUS_DEVICE_BUSY;
    c->Owner = file; WdfDeviceSetStaticStopRemove(c->Device, FALSE);
#define REQUIRE(call) do { s = (call); if (s != STATUS_SUCCESS) goto Fail; } while (0)
    c->Status.Phase = 1; REQUIRE(Graph(c));
    REQUIRE(Open(c, PI5_FCLK_NAME, GENERIC_READ | GENERIC_WRITE, &c->Clock));
    REQUIRE(Open(c, PI5_PM_NAME, GENERIC_READ | GENERIC_WRITE, &c->Pm));
    REQUIRE(Clock(c, IOCTL_PI5_FCLK_ACQUIRE, 0)); c->ClockLease = TRUE;
    REQUIRE(Pm(c, IOCTL_PI5_PM_ACQUIRE)); c->PmLease = TRUE;
    c->Status.Phase = 2; REQUIRE(Clock(c, IOCTL_PI5_FCLK_SET_STATE, 1));
    c->Powered = TRUE;
    sms = Read(c, V3dSms, 0x400); c->Status.SmsInitial = sms;
    /* The audited REE view has old/new mode 0, while TEE sees old/new mode
     * 1 (0x50). Never change a mode or clear a lock to acquire ownership. */
    if ((sms & 0x100000f0u) != 0x50 ||
        (Read(c, V3dSms, 0) & 0x100000f0u) ||
        ((sms & 15) != 0 && (sms & 15) != 13)) {
        s = STATUS_DEVICE_BUSY; goto Fail;
    }
    if ((sms & 15) == 13) {
        c->Woke = TRUE; Write(c, V3dSms, 0x400, 1u << 29);
        s = Hw(V3dPoll(&c->Io, V3dSms, 0x400, 15, 0));
        if (s != STATUS_SUCCESS) return Fault(c, s);
    }
    c->Status.Phase = 3; Snapshot(c);
    if (c->Status.TechVersion != 71 || ((c->Status.HubIdent[1] >> 8) & 15) != 1 ||
        c->Status.HubIdent[0] != 0x42554856 ||
        (c->Status.CoreIdent[0] & 0xffffff) != 0x443356 ||
        !(c->Status.HubIdent[2] & 0x100) ||
        c->Status.PaBits != 36 || c->Status.VaBits < 32 || c->Status.VaBits > 36) {
        s = STATUS_NOT_SUPPORTED; goto Fail;
    }
    REQUIRE(Hw(V3dIdle(&c->Io)));
    if ((Read(c, V3dHub, V3D_MMU_CTL) & 1) ||
        (Read(c, V3dHub, V3D_GMP_STATUS) & V3D_GMP_BUSY) ||
        (Read(c, V3dHub, V3D_GMP_CONFIG) & 1)) { s = STATUS_DEVICE_BUSY; goto Fail; }
    c->Claimed = TRUE;
    c->Status.Phase = 4;
    REQUIRE(Allocate(c, &c->Table, V3D_TABLE_BYTES));
    REQUIRE(Allocate(c, &c->Trap, V3D_PAGE));
    REQUIRE(Allocate(c, &c->Source, V3D_DATA_BYTES));
    REQUIRE(Allocate(c, &c->Destination, V3D_DATA_BYTES));
    REQUIRE(Allocate(c, &c->Code, V3D_PAGE));
    REQUIRE(Allocate(c, &c->Uniform, V3D_PAGE));
    REQUIRE(Allocate(c, &c->Tile, V3D_TILE_BYTES));
    REQUIRE(Allocate(c, &c->TileState, V3D_PAGE));
    c->Status.TableDma = (ULONGLONG)c->Table.Dma.QuadPart + V3D_PAGE;
    c->Status.SourceDma = (ULONGLONG)c->Source.Dma.QuadPart + V3D_PAGE;
    c->Status.DestinationDma = (ULONGLONG)c->Destination.Dma.QuadPart + V3D_PAGE;
    c->ComputeStatus.CodeDma = (ULONGLONG)c->Code.Dma.QuadPart + V3D_PAGE;
    c->ComputeStatus.UniformDma = (ULONGLONG)c->Uniform.Dma.QuadPart + V3D_PAGE;
    c->Table.Cpu[1024 + V3D_CODE_VA / V3D_PAGE] = V3dPte(c->ComputeStatus.CodeDma, 0);
    c->Table.Cpu[1024 + V3D_UNIFORM_VA / V3D_PAGE] = V3dPte(c->ComputeStatus.UniformDma, 0);
    c->Table.Cpu[1024 + V3D_TILE_STATE_VA / V3D_PAGE] = V3dPte((ULONGLONG)c->TileState.Dma.QuadPart + V3D_PAGE, 1);
    for (i = 0; i < V3D_TILE_BYTES / V3D_PAGE; ++i)
        c->Table.Cpu[1024 + V3D_TILE_VA / V3D_PAGE + i] = V3dPte((ULONGLONG)c->Tile.Dma.QuadPart + V3D_PAGE + i * V3D_PAGE, 1);
    for (i = 0; i < V3D_DATA_BYTES / V3D_PAGE; ++i) {
        c->Table.Cpu[1024 + V3D_SOURCE_VA / V3D_PAGE + i] = V3dPte(c->Status.SourceDma + i * V3D_PAGE, 0);
        c->Table.Cpu[1024 + V3D_DEST_VA / V3D_PAGE + i] = V3dPte(c->Status.DestinationDma + i * V3D_PAGE, 1);
    }
    REQUIRE(Reset(c)); c->Status.Phase = 5; Snapshot(c); return STATUS_SUCCESS;
Fail:
    c->Status.LastStatus = (ULONG)s;
    if (!BootFault) { clean = End(c); if (clean != STATUS_SUCCESS) return clean; }
    return s;
#undef REQUIRE
}
static NTSTATUS MemoryBegin(V3D_CONTEXT *c, WDFFILEOBJECT file)
{
    NTSTATUS s = Begin(c, file);
    ULONG i;
    if (s != STATUS_SUCCESS) return s;
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    for (i = 0; i < V3D_TABLE_BYTES / 4; ++i) c->Table.Cpu[1024 + i] = 0;
    KeMemoryBarrier();
    s = Hw(V3dMmuFlush(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    c->MemorySession = TRUE;
    return STATUS_SUCCESS;
}
static V3D_OBJECT *Object(V3D_CONTEXT *c, ULONG handle)
{
    ULONG i;
    if (!handle) return NULL;
    for (i = 0; i < PI5_V3D_BUFFER_LIMIT; ++i)
        if (c->Objects[i].Handle == handle) return &c->Objects[i];
    return NULL;
}
static NTSTATUS ObjectCreate(V3D_CONTEXT *c, const PI5_V3D_BUFFER *in, PI5_V3D_BUFFER *out)
{
    V3D_OBJECT *o = NULL;
    ULONG i, page, size = in->Bytes, flags = in->Flags;
    ULONGLONG address = 0x101000;
    NTSTATUS s;
    if (!size || size > PI5_V3D_BUFFER_MAX_BYTES || (size & 4095) ||
        flags & ~PI5_V3D_BUFFER_WRITABLE || in->Handle || in->Address || in->Reserved)
        return STATUS_INVALID_PARAMETER;
    if (size > PI5_V3D_MEMORY_MAX_BYTES - c->ObjectBytes || c->ObjectSerial == UINT32_MAX)
        return STATUS_INSUFFICIENT_RESOURCES;
    for (i = 0; i < PI5_V3D_BUFFER_LIMIT; ++i)
        if (!c->Objects[i].Handle) { o = &c->Objects[i]; break; }
    if (!o) return STATUS_INSUFFICIENT_RESOURCES;
    for (;;) {
        BOOLEAN overlap = FALSE;
        if (address + size + V3D_PAGE > 0xf0000000ull) return STATUS_INSUFFICIENT_RESOURCES;
        for (i = 0; i < PI5_V3D_BUFFER_LIMIT; ++i) {
            V3D_OBJECT *other = &c->Objects[i];
            if (!other->Handle) continue;
            if (address - V3D_PAGE < (ULONGLONG)other->Address + other->Bytes + V3D_PAGE &&
                address + size + V3D_PAGE > (ULONGLONG)other->Address - V3D_PAGE) {
                address = (ULONGLONG)other->Address + other->Bytes + 2 * V3D_PAGE;
                overlap = TRUE; break;
            }
        }
        if (!overlap) break;
    }
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Allocate(c, &o->Buffer, size); if (s != STATUS_SUCCESS) return s;
    o->Handle = ++c->ObjectSerial; o->Address = (ULONG)address; o->Bytes = size; o->Flags = flags;
    c->ObjectBytes += size;
    for (page = 0; page < size / V3D_PAGE; ++page)
        c->Table.Cpu[1024 + o->Address / V3D_PAGE + page] =
            V3dPte((ULONGLONG)o->Buffer.Dma.QuadPart + V3D_PAGE + page * V3D_PAGE, !!flags);
    KeMemoryBarrier();
    s = Hw(V3dMmuFlush(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    RtlZeroMemory(out, sizeof(*out)); out->Version = 1; out->Handle = o->Handle;
    out->Address = o->Address; out->Bytes = size; out->Flags = flags;
    return STATUS_SUCCESS;
}
static NTSTATUS ObjectDestroy(V3D_CONTEXT *c, const PI5_V3D_BUFFER *in)
{
    V3D_OBJECT *o = Object(c, in->Handle);
    ULONG page;
    NTSTATUS s;
    if (!o || in->Address || in->Bytes || in->Flags || in->Reserved) return STATUS_INVALID_PARAMETER;
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    for (page = 0; page < o->Bytes / V3D_PAGE; ++page)
        c->Table.Cpu[1024 + o->Address / V3D_PAGE + page] = 0;
    KeMemoryBarrier();
    s = Hw(V3dMmuFlush(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Hw(V3dInvalidate(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    c->ObjectBytes -= o->Bytes; FreeBuffer(c, &o->Buffer); RtlZeroMemory(o, sizeof(*o));
    return STATUS_SUCCESS;
}
static NTSTATUS ObjectTransfer(V3D_CONTEXT *c, const PI5_V3D_TRANSFER *in, void *data, BOOLEAN write)
{
    V3D_OBJECT *o = Object(c, in->Handle);
    ULONG i;
    ULONG *words = data;
    NTSTATUS s;
    if (!o || !in->Bytes || in->Bytes > PI5_V3D_TRANSFER_MAX_BYTES ||
        ((in->Offset | in->Bytes) & 3) || in->Offset > o->Bytes || in->Bytes > o->Bytes - in->Offset)
        return STATUS_INVALID_PARAMETER;
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    KeMemoryBarrier();
    for (i = 0; i < in->Bytes / 4; ++i) {
        if (write) o->Buffer.Cpu[1024 + in->Offset / 4 + i] = words[i];
        else words[i] = o->Buffer.Cpu[1024 + in->Offset / 4 + i];
    }
    KeMemoryBarrier();
    return STATUS_SUCCESS;
}
static NTSTATUS Wait(V3D_CONTEXT *c, ULONG hub, ULONG core)
{
    ULONGLONG deadline = KeQueryInterruptTime() + 5000000ull;
    for (;;) {
        LARGE_INTEGER timeout;
        ULONGLONG now;
        if ((c->Pending[0] & V3D_MMU_FAULT_IRQS) ||
            (c->Pending[1] & 4) ||
            (Read(c, V3dHub, V3D_MMU_CTL) & V3D_MMU_FAULTS)) return STATUS_DEVICE_HARDWARE_ERROR;
        if (((ULONG)c->Pending[0] & hub) == hub && ((ULONG)c->Pending[1] & core) == core)
            return STATUS_SUCCESS;
        now = KeQueryInterruptTime(); if (now >= deadline) return STATUS_IO_TIMEOUT;
        timeout.QuadPart = -(LONGLONG)(deadline - now);
        (void)KeWaitForSingleObject(&c->Wake, Executive, KernelMode, FALSE, &timeout);
        KeClearEvent(&c->Wake);
    }
}
static VOID Arm(V3D_CONTEXT *c)
{
    Mask(c, FALSE);
    InterlockedExchange(&c->Pending[0], 0); InterlockedExchange(&c->Pending[1], 0);
    KeClearEvent(&c->Wake); Mask(c, TRUE);
}
static NTSTATUS ObjectCopy(V3D_CONTEXT *c, const PI5_V3D_BUFFER_COPY *in, PI5_V3D_BUFFER_COPY *out)
{
    V3D_OBJECT *source = Object(c, in->Source), *destination = Object(c, in->Destination);
    ULONG bytes;
    NTSTATUS s;
    if (!source || !destination || source == destination || !(destination->Flags & PI5_V3D_BUFFER_WRITABLE) ||
        !in->Width || in->Width > 4096 || !in->Height || in->Height > 4096 ||
        ((in->SourceOffset | in->DestinationOffset) & 63) || in->Reserved || in->Fence)
        return STATUS_INVALID_PARAMETER;
    bytes = in->Width * in->Height * 4;
    if (in->SourceOffset > source->Bytes || bytes > source->Bytes - in->SourceOffset ||
        in->DestinationOffset > destination->Bytes || bytes > destination->Bytes - in->DestinationOffset)
        return STATUS_INVALID_PARAMETER;
    if (c->MemorySubmitted == UINT64_MAX) return STATUS_INTEGER_OVERFLOW;
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    KeMemoryBarrier();
    s = Hw(V3dInvalidate(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    Write(c, V3dHub, V3D_GMP_CONFIG, 0);
    Arm(c); ++c->MemorySubmitted; c->Status.Phase = 50;
    V3dSubmitCopyAt(&c->Io, in->Width, in->Height, source->Address + in->SourceOffset,
                   destination->Address + in->DestinationOffset);
    s = Wait(c, V3D_TFU_DONE, 0);
    Snapshot(c);
    if (s != STATUS_SUCCESS) { Mask(c, FALSE); return Fault(c, s); }
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    if ((ULONG)c->Pending[0] != V3D_TFU_DONE || c->Pending[1] ||
        Read(c, V3dHub, V3D_MMU_CTL) & V3D_MMU_FAULTS) return Fault(c, STATUS_DEVICE_HARDWARE_ERROR);
    KeMemoryBarrier(); s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    c->MemoryCompleted = c->MemorySubmitted; out->Fence = c->MemoryCompleted;
    ++c->Status.Copies; c->Status.Phase = 5;
    return STATUS_SUCCESS;
}
static BOOLEAN ObjectRange(V3D_CONTEXT *c, ULONG address, ULONG bytes, BOOLEAN writable)
{
    ULONG i;
    if (!bytes) return FALSE;
    for (i = 0; i < PI5_V3D_BUFFER_LIMIT; ++i) {
        V3D_OBJECT *o = &c->Objects[i];
        if (o->Handle && !!(o->Flags & PI5_V3D_BUFFER_WRITABLE) == !!writable &&
            address >= o->Address && address - o->Address < o->Bytes &&
            bytes <= o->Bytes - (address - o->Address)) return TRUE;
    }
    return FALSE;
}
static NTSTATUS ObjectSubmit(V3D_CONTEXT *c, const PI5_V3D_SUBMIT_CL *in, PI5_V3D_SUBMIT_CL *out)
{
    PI5_V3D_RENDER_STATUS *q = &c->RenderStatus;
    BOOLEAN bin = in->BclStart != in->BclEnd;
    NTSTATUS s;
    if (in->Reserved || in->Fence || in->RclEnd <= in->RclStart ||
        !ObjectRange(c, in->RclStart, in->RclEnd - in->RclStart, FALSE))
        return STATUS_INVALID_PARAMETER;
    if (bin) {
        if (in->BclEnd <= in->BclStart ||
            !ObjectRange(c, in->BclStart, in->BclEnd - in->BclStart, FALSE) ||
            ((in->TileAddress | in->TileBytes | in->StateAddress | in->StateBytes) & (V3D_PAGE - 1)) ||
            in->TileBytes < V3D_TILE_BYTES || in->StateBytes < V3D_PAGE ||
            !ObjectRange(c, in->TileAddress, in->TileBytes, TRUE) ||
            !ObjectRange(c, in->StateAddress, in->StateBytes, TRUE)) return STATUS_INVALID_PARAMETER;
    } else if (in->BclStart || in->BclEnd || in->TileAddress || in->TileBytes ||
               in->StateAddress || in->StateBytes) return STATUS_INVALID_PARAMETER;
    if (c->MemorySubmitted == UINT64_MAX || q->Submitted == UINT64_MAX) return STATUS_INTEGER_OVERFLOW;
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    KeMemoryBarrier();
    s = Hw(V3dInvalidate(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    Write(c, V3dHub, V3D_GMP_CONFIG, 0);
    ++c->MemorySubmitted; ++q->Submitted; ++q->Invalidations;
    q->CommandBytes = in->RclEnd - in->RclStart;
    q->BinBytes = in->BclEnd - in->BclStart;
    if (bin) {
        q->BinBefore = Read(c, V3dCore, 0x134);
        Arm(c); c->Status.Phase = 51;
        V3dSubmitBinAt(&c->Io, in->BclStart, in->BclEnd, in->TileAddress, in->TileBytes, in->StateAddress);
        s = Wait(c, 0, 2);
        q->BinAfter = Read(c, V3dCore, 0x134);
        q->BinCurrent = Read(c, V3dCore, 0x110); q->BinEnd = Read(c, V3dCore, 0x108);
        Snapshot(c);
        if (s != STATUS_SUCCESS) { Mask(c, FALSE); return Fault(c, s); }
        if (q->BinAfter == q->BinBefore || c->Pending[0] || (ULONG)c->Pending[1] != 2)
            return Fault(c, STATUS_DEVICE_HARDWARE_ERROR);
        s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
        s = Hw(V3dInvalidate(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
        Write(c, V3dHub, V3D_GMP_CONFIG, 0); ++q->Invalidations;
    }
    q->CtBefore = Read(c, V3dCore, 0x104); q->FramesBefore = Read(c, V3dCore, 0x138);
    Arm(c); c->Status.Phase = 52;
    V3dSubmitRenderAt(&c->Io, in->RclStart, in->RclEnd);
    s = Wait(c, 0, V3D_RENDER_DONE);
    q->CtAfter = Read(c, V3dCore, 0x104);
    q->Current = Read(c, V3dCore, 0x114); q->End = Read(c, V3dCore, 0x10c);
    q->QueueCurrent = Read(c, V3dCore, 0x164); q->QueueEnd = Read(c, V3dCore, 0x16c);
    q->FramesAfter = Read(c, V3dCore, 0x138); Snapshot(c);
    if (s != STATUS_SUCCESS) { Mask(c, FALSE); return Fault(c, s); }
    if (q->FramesAfter == q->FramesBefore) return Fault(c, STATUS_DEVICE_HARDWARE_ERROR);
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    if (c->Pending[0] || (ULONG)c->Pending[1] != V3D_RENDER_DONE ||
        Read(c, V3dHub, V3D_MMU_CTL) & V3D_MMU_FAULTS)
        return Fault(c, STATUS_DEVICE_HARDWARE_ERROR);
    KeMemoryBarrier(); s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    c->MemoryCompleted = c->MemorySubmitted; out->Fence = c->MemoryCompleted;
    q->Completed = q->Submitted; ++q->Jobs; c->Status.Phase = 5;
    return STATUS_SUCCESS;
}
static NTSTATUS InterruptCheck(V3D_CONTEXT *c)
{
    NTSTATUS s;
    c->Status.Phase = 6; Arm(c);
    Write(c, V3dHub, V3D_INT_SET, 2); Write(c, V3dCore, V3D_INT_SET, 1);
    s = Wait(c, 2, 1);
    if (s != STATUS_SUCCESS) return Fault(c, s);
    ++c->Status.InterruptChecks; return STATUS_SUCCESS;
}
static NTSTATUS Copy(V3D_CONTEXT *c, const PI5_V3D_IMAGE *in, PI5_V3D_IMAGE *out)
{
    ULONG i, words = in->Width * in->Height;
    NTSTATUS s;
    c->Status.Phase = 7;
    s = Hw(V3dIdle(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    for (i = 0; i < PI5_V3D_WORDS; ++i) {
        c->Source.Cpu[1024 + i] = i < words ? in->Pixels[i] : GUARD;
        c->Destination.Cpu[1024 + i] = GUARD;
    }
    KeMemoryBarrier(); Arm(c);
    c->Status.Phase = 8; V3dSubmitCopy(&c->Io, in->Width, in->Height);
    s = Wait(c, V3D_TFU_DONE, 0); Snapshot(c);
    if (s != STATUS_SUCCESS) { Mask(c, FALSE); return Fault(c, s); }
    s = Hw(V3dPoll(&c->Io, V3dHub, V3D_TFU_CS, 1, 0));
    if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    KeMemoryBarrier(); s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    for (i = 0; i < PI5_V3D_WORDS; ++i) {
        if (c->Source.Cpu[1024 + i] != (i < words ? in->Pixels[i] : GUARD) ||
            c->Destination.Cpu[1024 + i] != (i < words ? in->Pixels[i] : GUARD))
            return Fault(c, STATUS_DATA_ERROR);
    }
    /* Both request descriptors may refer to the same buffered allocation. */
    for (i = 0; i < PI5_V3D_WORDS; ++i) out->Pixels[i] = i < words ? c->Destination.Cpu[1024 + i] : 0;
    Write(c, V3dHub, V3D_GMP_CONFIG, 0);
    ++c->Status.Copies; c->Status.Phase = 5; return STATUS_SUCCESS;
}

static NTSTATUS Compute(V3D_CONTEXT *c, const PI5_V3D_COMPUTE *in, PI5_V3D_COMPUTE *out)
{
    const uint64_t *shader = in->Operation == PI5_V3D_XOR_ADD ? V3dShaderAdd : V3dShaderSub;
    const ULONG uniforms[] = {V3D_SOURCE_VA, V3D_DEST_VA, in->XorValue, in->Operand};
    PI5_V3D_COMPUTE_STATUS *q = &c->ComputeStatus;
    ULONG i, expected, word;
    NTSTATUS s;
    C_ASSERT(sizeof(V3dShaderAdd) == sizeof(V3dShaderSub));
    C_ASSERT(sizeof(V3dShaderAdd) <= V3D_PAGE);
    if (q->Submitted == UINT64_MAX) return STATUS_INTEGER_OVERFLOW;
    c->Status.Phase = 30;
    s = Hw(V3dIdle(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    for (i = 0; i < PI5_V3D_WORDS; ++i) {
        c->Source.Cpu[1024 + i] = i < PI5_V3D_COMPUTE_LANES ? in->Values[i] : GUARD;
        expected = GUARD;
        if (i < PI5_V3D_COMPUTE_LANES) {
            word = in->Values[i] ^ in->XorValue;
            expected = ~(in->Operation == PI5_V3D_XOR_ADD ? word + in->Operand : word - in->Operand);
        }
        /* Every active output must change, even when its result is GUARD. */
        c->Destination.Cpu[1024 + i] = expected;
    }
    for (i = 0; i < V3D_PAGE / 4; ++i) {
        word = i < sizeof(V3dShaderAdd) / 4 ? (ULONG)(shader[i / 2] >> ((i & 1) * 32)) : GUARD;
        c->Code.Cpu[1024 + i] = word;
        c->Uniform.Cpu[1024 + i] = i < RTL_NUMBER_OF(uniforms) ? uniforms[i] : GUARD;
    }
    KeMemoryBarrier();
    s = Hw(V3dInvalidate(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    ++q->Invalidations; q->CodeBytes = sizeof(V3dShaderAdd);
    Arm(c);
    q->CsdBefore = Read(c, V3dCore, V3D_CSD_STATUS);
    ++q->Submitted; c->Status.Phase = 31;
    V3dSubmitCompute(&c->Io, q->IpRevision);
    s = Wait(c, 0, V3D_CSD_DONE);
    q->CsdAfter = Read(c, V3dCore, V3D_CSD_STATUS); Snapshot(c);
    if (s != STATUS_SUCCESS) { Mask(c, FALSE); return Fault(c, s); }
    /* CSDDONE completes the sole outstanding dispatch, as in Linux's CSD
     * fence path. The legacy 4.2 NUM_COMPLETED bit layout is not valid on
     * 7.1; retain raw status for diagnostics instead of decoding that field.
     * TMU/L2 writeback and AXI drain are still required before completion. */
    c->Io.CsdIdle = q->CsdAfter;
    c->Status.Phase = 32;
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    if (c->Pending[0] || (ULONG)c->Pending[1] != V3D_CSD_DONE ||
        Read(c, V3dHub, V3D_MMU_CTL) & V3D_MMU_FAULTS)
        return Fault(c, STATUS_DEVICE_HARDWARE_ERROR);
    KeMemoryBarrier(); s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    for (i = 0; i < PI5_V3D_WORDS; ++i) {
        word = i < PI5_V3D_COMPUTE_LANES ? in->Values[i] : GUARD;
        expected = word;
        if (i < PI5_V3D_COMPUTE_LANES) {
            expected ^= in->XorValue;
            expected = in->Operation == PI5_V3D_XOR_ADD ? expected + in->Operand : expected - in->Operand;
        }
        if (c->Source.Cpu[1024 + i] != word || c->Destination.Cpu[1024 + i] != expected)
            return Fault(c, STATUS_DATA_ERROR);
    }
    for (i = 0; i < V3D_PAGE / 4; ++i) {
        word = i < sizeof(V3dShaderAdd) / 4 ? (ULONG)(shader[i / 2] >> ((i & 1) * 32)) : GUARD;
        if (c->Code.Cpu[1024 + i] != word ||
            c->Uniform.Cpu[1024 + i] != (i < RTL_NUMBER_OF(uniforms) ? uniforms[i] : GUARD))
            return Fault(c, STATUS_DATA_ERROR);
    }
    for (i = 0; i < PI5_V3D_COMPUTE_LANES; ++i) out->Values[i] = c->Destination.Cpu[1024 + i];
    Write(c, V3dHub, V3D_GMP_CONFIG, 0);
    q->Completed = q->Submitted; out->Fence = q->Completed;
    ++q->Jobs; c->Status.Phase = 5; return STATUS_SUCCESS;
}

static NTSTATUS Render(V3D_CONTEXT *c, const PI5_V3D_RENDER *in, PI5_V3D_RENDER *out, BOOLEAN triangle)
{
    ULONG command[32] = {0};
    ULONG bytes, i, expected, words = in->Width * in->Height;
    const ULONG color = in->Color;
    PI5_V3D_RENDER_STATUS *q = &c->RenderStatus;
    NTSTATUS s;
    if (q->Submitted == UINT64_MAX) return STATUS_INTEGER_OVERFLOW;
    if (triangle && c->ComputeStatus.IpRevision != 10) return STATUS_NOT_SUPPORTED;
    bytes = triangle ? V3dTriangleRclBytes() :
        V3dBuildClear((uint8_t *)command, sizeof(command), in->Width, in->Height, color);
    if (!bytes) return STATUS_INVALID_PARAMETER;
    c->Status.Phase = 40;
    s = Hw(V3dIdle(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    for (i = 0; i < PI5_V3D_WORDS; ++i) {
        c->Source.Cpu[1024 + i] = triangle ? V3dTriangleSourceWord(i) : GUARD;
        expected = triangle ? V3dTrianglePixel(i, color) : color;
        c->Destination.Cpu[1024 + i] = i < words ? ~expected : GUARD;
    }
    for (i = 0; i < V3D_PAGE / 4; ++i) {
        c->Code.Cpu[1024 + i] = triangle ? V3dTriangleCodeWord(i, color) :
            (i < (bytes + 3) / 4 ? command[i] : GUARD);
        c->Uniform.Cpu[1024 + i] = triangle ? V3dTriangleUniformWord(i) : GUARD;
    }
    if (triangle) {
        for (i = 0; i < V3D_TILE_BYTES / 4; ++i) c->Tile.Cpu[1024 + i] = 0;
        for (i = 0; i < V3D_PAGE / 4; ++i) c->TileState.Cpu[1024 + i] = 0;
    }
    KeMemoryBarrier();
    s = Hw(V3dInvalidate(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    ++q->Invalidations; q->CommandBytes = bytes;
    q->BinBytes = triangle ? V3dTriangleBclBytes() : 0;
    ++q->Submitted;
    if (triangle) {
        q->BinBefore = Read(c, V3dCore, 0x134);
        Arm(c); c->Status.Phase = 43;
        V3dSubmitBin(&c->Io);
        s = Wait(c, 0, 2);
        q->BinAfter = Read(c, V3dCore, 0x134);
        q->BinCurrent = Read(c, V3dCore, 0x110); q->BinEnd = Read(c, V3dCore, 0x108);
        Snapshot(c);
        if (s != STATUS_SUCCESS) { Mask(c, FALSE); return Fault(c, s); }
        if (q->BinAfter == q->BinBefore || c->Pending[0] || (ULONG)c->Pending[1] != 2)
            return Fault(c, STATUS_DEVICE_HARDWARE_ERROR);
        c->Status.Phase = 44;
        s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
        Write(c, V3dHub, V3D_GMP_CONFIG, 0);
        s = Hw(V3dInvalidate(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
        ++q->Invalidations;
    }
    q->CtBefore = Read(c, V3dCore, 0x104);
    q->FramesBefore = Read(c, V3dCore, 0x138);
    Arm(c); c->Status.Phase = 41;
    V3dSubmitRender(&c->Io, bytes);
    s = Wait(c, 0, V3D_RENDER_DONE);
    q->CtAfter = Read(c, V3dCore, 0x104);
    q->Current = Read(c, V3dCore, 0x114); q->End = Read(c, V3dCore, 0x10c);
    q->QueueCurrent = Read(c, V3dCore, 0x164); q->QueueEnd = Read(c, V3dCore, 0x16c);
    q->FramesAfter = Read(c, V3dCore, 0x138); Snapshot(c);
    if (s != STATUS_SUCCESS) { Mask(c, FALSE); return Fault(c, s); }
    if (q->FramesAfter == q->FramesBefore) return Fault(c, STATUS_DEVICE_HARDWARE_ERROR);
    c->Status.Phase = 42;
    s = Hw(V3dDrain(&c->Io)); if (s != STATUS_SUCCESS) return Fault(c, s);
    if (c->Pending[0] || (ULONG)c->Pending[1] != V3D_RENDER_DONE ||
        Read(c, V3dHub, V3D_MMU_CTL) & V3D_MMU_FAULTS)
        return Fault(c, STATUS_DEVICE_HARDWARE_ERROR);
    KeMemoryBarrier(); s = Guards(c); if (s != STATUS_SUCCESS) return Fault(c, s);
    for (i = 0; i < PI5_V3D_WORDS; ++i) {
        expected = i < words ? (triangle ? V3dTrianglePixel(i, color) : color) : GUARD;
        if (c->Destination.Cpu[1024 + i] != expected) {
            q->MismatchIndex = i; q->Observed = c->Destination.Cpu[1024 + i]; q->Expected = expected;
            return Fault(c, STATUS_DATA_ERROR);
        }
        if (c->Source.Cpu[1024 + i] != (triangle ? V3dTriangleSourceWord(i) : GUARD))
            return Fault(c, STATUS_DATA_ERROR);
    }
    for (i = 0; i < V3D_PAGE / 4; ++i) {
        expected = triangle ? V3dTriangleCodeWord(i, color) : (i < (bytes + 3) / 4 ? command[i] : GUARD);
        if (c->Code.Cpu[1024 + i] != expected ||
            c->Uniform.Cpu[1024 + i] != (triangle ? V3dTriangleUniformWord(i) : GUARD))
            return Fault(c, STATUS_DATA_ERROR);
    }
    for (i = 0; i < PI5_V3D_WORDS; ++i) out->Pixels[i] = i < words ? c->Destination.Cpu[1024 + i] : 0;
    Write(c, V3dHub, V3D_GMP_CONFIG, 0);
    q->Completed = q->Submitted; out->Fence = q->Completed;
    ++q->Jobs; c->Status.Phase = 5; return STATUS_SUCCESS;
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
    V3D_CONTEXT *c;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_PNPPOWER_EVENT_CALLBACKS p;
    WDF_IO_QUEUE_CONFIG q;
    WDF_FILEOBJECT_CONFIG f;
    WDF_INTERRUPT_CONFIG irq;
    NTSTATUS s;
    ULONG i;
    DECLARE_CONST_UNICODE_STRING(name, PI5_V3D_NAME);
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\Pi5V3d");
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GR;;;BA)");
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, TRUE);
    s = WdfDeviceInitAssignName(init, &name); if (!NT_SUCCESS(s)) return s;
    s = WdfDeviceInitAssignSDDLString(init, &sddl); if (!NT_SUCCESS(s)) return s;
    WDF_FILEOBJECT_CONFIG_INIT(&f, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, Cleanup);
    WdfDeviceInitSetFileObjectConfig(init, &f, WDF_NO_OBJECT_ATTRIBUTES);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);
    p.EvtDevicePrepareHardware = Prepare; p.EvtDeviceReleaseHardware = ReleaseHardware;
    p.EvtDeviceD0Entry = Entry; p.EvtDeviceD0Exit = Exit;
    p.EvtDeviceQueryStop = QueryStop; p.EvtDeviceQueryRemove = QueryRemove;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &p);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, V3D_CONTEXT); a.ExecutionLevel = WdfExecutionLevelPassive;
    s = WdfDeviceCreate(&init, &a, &device); if (!NT_SUCCESS(s)) return s;
    c = Context(device); c->Device = device;
    c->Status.Version = 1; c->Status.Debug = DBG;
    c->Io.Context = c; c->Io.Read = Read; c->Io.Write = Write; c->Io.Delay = Delay;
    KeInitializeEvent(&c->Wake, NotificationEvent, FALSE);
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = device;
    s = WdfWaitLockCreate(&a, &c->Lock); if (!NT_SUCCESS(s)) return s;
    for (i = 0; i < 2; ++i) {
        WDF_INTERRUPT_CONFIG_INIT(&irq, Isr, Dpc);
        irq.AutomaticSerialization = FALSE;
        irq.EvtInterruptEnable = InterruptEnable; irq.EvtInterruptDisable = InterruptDisable;
        s = WdfInterruptCreate(device, &irq, WDF_NO_OBJECT_ATTRIBUTES, &c->Interrupt[i]);
        if (!NT_SUCCESS(s)) return s;
    }
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q, WdfIoQueueDispatchParallel);
    q.PowerManaged = WdfTrue; q.EvtIoDeviceControl = Control;
    s = WdfIoQueueCreate(device, &q, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(s)) return s;
    return WdfDeviceCreateSymbolicLink(device, &link);
}
_Use_decl_annotations_
NTSTATUS Prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    V3D_CONTEXT *c = Context(device);
    ULONG i, j, found = 0, irqs = 0, maps;
    DEVICE_DESCRIPTION d = {0};
    NTSTATUS s = Marker(FALSE);
    UNREFERENCED_PARAMETER(raw);
    if (BootFault || NT_SUCCESS(s)) return STATUS_DEVICE_HARDWARE_ERROR;
    if (s != STATUS_OBJECT_NAME_NOT_FOUND && s != STATUS_OBJECT_PATH_NOT_FOUND) return s;
    for (i = 0; i < WdfCmResourceListGetCount(translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeNull || r->Type == CmResourceTypeDevicePrivate) continue;
        if (r->Type == CmResourceTypeInterrupt) { ++irqs; continue; }
        if (r->Type != CmResourceTypeMemory) return STATUS_DEVICE_CONFIGURATION_ERROR;
        for (j = 0; j < 3; ++j) if ((ULONGLONG)r->u.Memory.Start.QuadPart == Bases[j] && r->u.Memory.Length == Lengths[j]) break;
        if (j == 3 || (found & (1u << j))) return STATUS_DEVICE_CONFIGURATION_ERROR;
        c->Registers[j] = MmMapIoSpaceEx(r->u.Memory.Start, Lengths[j], PAGE_READWRITE | PAGE_NOCACHE);
        if (!c->Registers[j]) return STATUS_INSUFFICIENT_RESOURCES;
        found |= 1u << j;
    }
    if (found != 7 || irqs != 2) return STATUS_DEVICE_CONFIGURATION_ERROR;
    d.Version = DEVICE_DESCRIPTION_VERSION3; d.Master = TRUE; d.ScatterGather = TRUE;
    d.InterfaceType = Internal; d.DmaAddressWidth = 36; d.MaximumLength = V3D_TABLE_BYTES + 8192;
    c->Adapter = IoGetDmaAdapter(WdfDeviceWdmGetPhysicalDevice(device), &d, &maps);
    if (!c->Adapter) return STATUS_INSUFFICIENT_RESOURCES;
    if (c->Adapter->DmaOperations->Size < FIELD_OFFSET(DMA_OPERATIONS, AllocateCommonBufferEx) +
        sizeof(c->Adapter->DmaOperations->AllocateCommonBufferEx) ||
        !c->Adapter->DmaOperations->AllocateCommonBufferEx) return STATUS_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS ReleaseHardware(WDFDEVICE device, WDFCMRESLIST translated)
{
    V3D_CONTEXT *c = Context(device);
    ULONG i;
    UNREFERENCED_PARAMETER(translated);
    WdfWaitLockAcquire(c->Lock, NULL);
    if (c->Owner) (void)Fault(c, STATUS_DEVICE_BUSY);
    c->Status.Online = 0;
    for (i = 0; i < 3; ++i) {
        if (c->Registers[i]) MmUnmapIoSpace(c->Registers[i], Lengths[i]);
        c->Registers[i] = NULL;
    }
    if (c->Adapter && !BootFault) { c->Adapter->DmaOperations->PutDmaAdapter(c->Adapter); c->Adapter = NULL; }
    /* A forced teardown never recycles uncertain DMA memory. The volatile
     * marker also blocks an owner from a subsequently loaded driver image. */
    WdfWaitLockRelease(c->Lock); return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    UNREFERENCED_PARAMETER(previous);
    Context(device)->Status.Online = !BootFault;
    return BootFault ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target)
{
    V3D_CONTEXT *c = Context(device);
    UNREFERENCED_PARAMETER(target);
    WdfWaitLockAcquire(c->Lock, NULL);
    c->Status.Online = 0;
    if (c->Owner) (void)End(c);
    WdfWaitLockRelease(c->Lock); return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS QueryStop(WDFDEVICE device)
{
    return Context(device)->Owner || BootFault ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS QueryRemove(WDFDEVICE device) { return QueryStop(device); }
_Use_decl_annotations_
VOID Cleanup(WDFFILEOBJECT file)
{
    V3D_CONTEXT *c = Context(WdfFileObjectGetDevice(file));
    WdfWaitLockAcquire(c->Lock, NULL);
    if (c->Owner == file) (void)End(c);
    WdfWaitLockRelease(c->Lock);
}
_Use_decl_annotations_
NTSTATUS InterruptEnable(WDFINTERRUPT irq, WDFDEVICE device)
{
    UNREFERENCED_PARAMETER(irq); UNREFERENCED_PARAMETER(device);
    return STATUS_SUCCESS; /* GPU stays untouched until a kernel session. */
}
_Use_decl_annotations_
NTSTATUS InterruptDisable(WDFINTERRUPT irq, WDFDEVICE device)
{
    V3D_CONTEXT *c = Context(device);
    ULONG unit = irq == c->Interrupt[0] ? 0 : 1;
    if (c->IrqLive[unit]) Write(c, unit, V3D_INT_MASK_SET, UINT32_MAX);
    c->IrqLive[unit] = 0; return STATUS_SUCCESS;
}
_Use_decl_annotations_
BOOLEAN Isr(WDFINTERRUPT irq, ULONG message)
{
    V3D_CONTEXT *c = Context(WdfInterruptGetDevice(irq));
    ULONG unit = irq == c->Interrupt[0] ? 0 : 1, bits;
    UNREFERENCED_PARAMETER(message);
    if (!c->IrqLive[unit]) return FALSE;
    bits = Read(c, unit, V3D_INT_STS) & (unit ? V3D_CORE_IRQS : V3D_HUB_IRQS);
    if (!bits) return FALSE;
    Write(c, unit, V3D_INT_CLR, bits);
    InterlockedOr(&c->Pending[unit], (LONG)bits); InterlockedIncrement(&c->InterruptCount[unit]);
    (void)WdfInterruptQueueDpcForIsr(irq); return TRUE;
}
_Use_decl_annotations_
VOID Dpc(WDFINTERRUPT irq, WDFOBJECT device)
{
    UNREFERENCED_PARAMETER(irq);
    KeSetEvent(&Context((WDFDEVICE)device)->Wake, IO_NO_INCREMENT, FALSE);
}
_Use_decl_annotations_
VOID Control(WDFQUEUE queue, WDFREQUEST request, size_t ol, size_t il, ULONG code)
{
    V3D_CONTEXT *c = Context(WdfIoQueueGetDevice(queue));
    WDFFILEOBJECT file = WdfRequestGetFileObject(request);
    PI5_V3D_REQUEST *in = NULL;
    PI5_V3D_IMAGE *image = NULL, *outImage = NULL;
    PI5_V3D_COMPUTE *compute = NULL, *outCompute = NULL;
    PI5_V3D_COMPUTE_STATUS *outComputeStatus;
    PI5_V3D_RENDER *render = NULL, *outRender = NULL;
    PI5_V3D_RENDER_STATUS *outRenderStatus;
    PI5_V3D_STATUS *out;
    NTSTATUS s;
    ULONG_PTR used = 0;
    LONGLONG timeout = WDF_REL_TIMEOUT_IN_SEC(5);
    s = WdfWaitLockAcquire(c->Lock, &timeout);
    if (s != STATUS_SUCCESS) { WdfRequestComplete(request, s == STATUS_TIMEOUT ? STATUS_IO_TIMEOUT : s); return; }
    if (code == IOCTL_PI5_V3D_QUERY) {
        if (il || ol != sizeof(*out)) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), (PVOID *)&out, NULL);
        if (s == STATUS_SUCCESS) { Snapshot(c); *out = c->Status; used = sizeof(*out); }
        goto Done;
    }
    if (code == IOCTL_PI5_V3D_COMPUTE_QUERY) {
        if (il || ol != sizeof(*outComputeStatus)) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = WdfRequestRetrieveOutputBuffer(request, ol, (PVOID *)&outComputeStatus, NULL);
        if (s == STATUS_SUCCESS) {
            c->ComputeStatus.Version = 1;
            *outComputeStatus = c->ComputeStatus; used = sizeof(*outComputeStatus);
        }
        goto Done;
    }
    if (code == IOCTL_PI5_V3D_RENDER_QUERY) {
        if (il || ol != sizeof(*outRenderStatus)) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = WdfRequestRetrieveOutputBuffer(request, ol, (PVOID *)&outRenderStatus, NULL);
        if (s == STATUS_SUCCESS) {
            c->RenderStatus.Version = 1;
            *outRenderStatus = c->RenderStatus; used = sizeof(*outRenderStatus);
        }
        goto Done;
    }
    if (WdfRequestGetRequestorMode(request) != KernelMode) { s = STATUS_ACCESS_DENIED; goto Done; }
    if (!file || !c->Status.Online) { s = STATUS_DEVICE_NOT_READY; goto Done; }
    if (BootFault) { s = STATUS_DEVICE_HARDWARE_ERROR; goto Done; }
    if (code == IOCTL_PI5_V3D_SUBMIT_CL) {
        PI5_V3D_SUBMIT_CL *cl, *result;
        if (c->Owner != file || !c->MemorySession) { s = STATUS_ACCESS_DENIED; goto Done; }
        if (il != sizeof(*cl) || ol != sizeof(*cl)) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = WdfRequestRetrieveInputBuffer(request, il, (PVOID *)&cl, NULL); if (s != STATUS_SUCCESS) goto Done;
        s = WdfRequestRetrieveOutputBuffer(request, ol, (PVOID *)&result, NULL); if (s != STATUS_SUCCESS) goto Done;
        if (cl->Version != 1) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = ObjectSubmit(c, cl, result); if (s == STATUS_SUCCESS) used = ol;
        c->Status.LastStatus = (ULONG)s; goto Done;
    }
    if (code == IOCTL_PI5_V3D_BUFFER_COPY) {
        PI5_V3D_BUFFER_COPY *copy, *result;
        if (c->Owner != file || !c->MemorySession) { s = STATUS_ACCESS_DENIED; goto Done; }
        if (il != sizeof(*copy) || ol != sizeof(*copy)) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = WdfRequestRetrieveInputBuffer(request, il, (PVOID *)&copy, NULL); if (s != STATUS_SUCCESS) goto Done;
        s = WdfRequestRetrieveOutputBuffer(request, ol, (PVOID *)&result, NULL); if (s != STATUS_SUCCESS) goto Done;
        if (copy->Version != 1) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = ObjectCopy(c, copy, result); if (s == STATUS_SUCCESS) used = ol;
        c->Status.LastStatus = (ULONG)s; goto Done;
    }
    if (code >= IOCTL_PI5_V3D_BUFFER_CREATE && code <= IOCTL_PI5_V3D_BUFFER_READ) {
        PVOID input, output = NULL;
        if (c->Owner != file || !c->MemorySession) { s = STATUS_ACCESS_DENIED; goto Done; }
        if (code == IOCTL_PI5_V3D_BUFFER_CREATE || code == IOCTL_PI5_V3D_BUFFER_DESTROY) {
            PI5_V3D_BUFFER *buffer;
            if (il != sizeof(*buffer) || ol != (code == IOCTL_PI5_V3D_BUFFER_CREATE ? sizeof(*buffer) : 0)) {
                s = STATUS_INVALID_PARAMETER; goto Done;
            }
            s = WdfRequestRetrieveInputBuffer(request, il, &input, NULL); if (s != STATUS_SUCCESS) goto Done;
            buffer = input;
            if (buffer->Version != 1) { s = STATUS_INVALID_PARAMETER; goto Done; }
            if (ol) {
                s = WdfRequestRetrieveOutputBuffer(request, ol, &output, NULL); if (s != STATUS_SUCCESS) goto Done;
                s = ObjectCreate(c, buffer, output); if (s == STATUS_SUCCESS) used = ol;
            } else s = ObjectDestroy(c, buffer);
        } else {
            PI5_V3D_TRANSFER transfer;
            BOOLEAN write = code == IOCTL_PI5_V3D_BUFFER_WRITE;
            if (il < sizeof(transfer)) { s = STATUS_INVALID_PARAMETER; goto Done; }
            s = WdfRequestRetrieveInputBuffer(request, il, &input, NULL); if (s != STATUS_SUCCESS) goto Done;
            transfer = *(PI5_V3D_TRANSFER *)input;
            if (transfer.Version != 1 || !transfer.Bytes || transfer.Bytes > PI5_V3D_TRANSFER_MAX_BYTES ||
                il != sizeof(transfer) + (write ? transfer.Bytes : 0) ||
                ol != (write ? 0 : sizeof(transfer) + transfer.Bytes)) {
                s = STATUS_INVALID_PARAMETER; goto Done;
            }
            if (!write) {
                s = WdfRequestRetrieveOutputBuffer(request, ol, &output, NULL); if (s != STATUS_SUCCESS) goto Done;
                *(PI5_V3D_TRANSFER *)output = transfer;
            }
            s = ObjectTransfer(c, &transfer, (PUCHAR)(write ? input : output) + sizeof(transfer), write);
            if (s == STATUS_SUCCESS) used = ol;
        }
        c->Status.LastStatus = (ULONG)s; goto Done;
    }
    if (c->MemorySession && (code == IOCTL_PI5_V3D_RENDER || code == IOCTL_PI5_V3D_TRIANGLE ||
        code == IOCTL_PI5_V3D_COMPUTE || code == IOCTL_PI5_V3D_COPY)) {
        s = STATUS_INVALID_DEVICE_STATE; goto Done;
    }
    if (code == IOCTL_PI5_V3D_RENDER || code == IOCTL_PI5_V3D_TRIANGLE) {
        if (il != sizeof(*render) || ol != sizeof(*render)) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = WdfRequestRetrieveInputBuffer(request, il, (PVOID *)&render, NULL); if (s != STATUS_SUCCESS) goto Done;
        s = WdfRequestRetrieveOutputBuffer(request, ol, (PVOID *)&outRender, NULL); if (s != STATUS_SUCCESS) goto Done;
        if (render->Version != 1 || !render->Width || render->Width > 64 ||
            !render->Height || render->Height > 32 || render->Fence) {
            s = STATUS_INVALID_PARAMETER; goto Done;
        }
        if (code == IOCTL_PI5_V3D_TRIANGLE &&
            (render->Width != 64 || render->Height != 32 || render->Color == 0xff00ff00)) {
            s = STATUS_INVALID_PARAMETER; goto Done;
        }
    } else if (code == IOCTL_PI5_V3D_COMPUTE) {
        if (il != sizeof(*compute) || ol != sizeof(*compute)) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = WdfRequestRetrieveInputBuffer(request, il, (PVOID *)&compute, NULL); if (s != STATUS_SUCCESS) goto Done;
        s = WdfRequestRetrieveOutputBuffer(request, ol, (PVOID *)&outCompute, NULL); if (s != STATUS_SUCCESS) goto Done;
        if (compute->Version != 1 || compute->Operation > PI5_V3D_XOR_SUB || compute->Fence) {
            s = STATUS_INVALID_PARAMETER; goto Done;
        }
    } else if (code == IOCTL_PI5_V3D_COPY) {
        if (il != sizeof(*image) || ol != sizeof(*image)) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = WdfRequestRetrieveInputBuffer(request, il, (PVOID *)&image, NULL); if (s != STATUS_SUCCESS) goto Done;
        s = WdfRequestRetrieveOutputBuffer(request, ol, (PVOID *)&outImage, NULL); if (s != STATUS_SUCCESS) goto Done;
        if (image->Version != 1 || image->Reserved || !image->Width || image->Width > 64 || !image->Height || image->Height > 32) {
            s = STATUS_INVALID_PARAMETER; goto Done;
        }
    } else {
        if (il != sizeof(*in) || ol) { s = STATUS_INVALID_PARAMETER; goto Done; }
        s = WdfRequestRetrieveInputBuffer(request, il, (PVOID *)&in, NULL); if (s != STATUS_SUCCESS) goto Done;
        if (in->Version != 1 || in->Reserved) { s = STATUS_INVALID_PARAMETER; goto Done; }
    }
    if (code != IOCTL_PI5_V3D_BEGIN && code != IOCTL_PI5_V3D_MEMORY_BEGIN && c->Owner != file) {
        s = STATUS_ACCESS_DENIED; goto Done;
    }
    switch (code) {
    case IOCTL_PI5_V3D_BEGIN: s = Begin(c, file); break;
    case IOCTL_PI5_V3D_MEMORY_BEGIN: s = MemoryBegin(c, file); break;
    case IOCTL_PI5_V3D_END: s = End(c); break;
    case IOCTL_PI5_V3D_RESET: s = Reset(c); break;
    case IOCTL_PI5_V3D_INTERRUPT_CHECK: s = InterruptCheck(c); break;
    case IOCTL_PI5_V3D_COPY:
        s = Copy(c, image, outImage); if (s == STATUS_SUCCESS) used = sizeof(*image); break;
    case IOCTL_PI5_V3D_COMPUTE:
        s = Compute(c, compute, outCompute); if (s == STATUS_SUCCESS) used = sizeof(*compute); break;
    case IOCTL_PI5_V3D_RENDER:
    case IOCTL_PI5_V3D_TRIANGLE:
        s = Render(c, render, outRender, code == IOCTL_PI5_V3D_TRIANGLE);
        if (s == STATUS_SUCCESS) used = sizeof(*render); break;
    default: s = STATUS_INVALID_DEVICE_REQUEST; break;
    }
    c->Status.LastStatus = (ULONG)s;
Done:
    WdfWaitLockRelease(c->Lock);
    WdfRequestCompleteWithInformation(request, s, used);
}
