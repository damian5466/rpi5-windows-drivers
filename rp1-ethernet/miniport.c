/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Experimental NDIS 6.30 miniport for ACPI\\RPI0001 only. */
#include <ntifs.h>
#include <ndis.h>
#include "gem.h"
#include "rp1-service.h"

#define TAG 'E1PR'
#define FILTERS (NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | \
                 NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST | \
                 NDIS_PACKET_TYPE_PROMISCUOUS)
#define MULTICAST_MAX 32u
#define RX_BATCH_SIZE 64u
#define AUTO_LINK (NDIS_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED | \
                   NDIS_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED | \
                   NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED)
#define STATS_SUPPORTED (NDIS_STATISTICS_XMIT_OK_SUPPORTED | NDIS_STATISTICS_RCV_OK_SUPPORTED | \
    NDIS_STATISTICS_XMIT_ERROR_SUPPORTED | NDIS_STATISTICS_RCV_ERROR_SUPPORTED | \
    NDIS_STATISTICS_RCV_NO_BUFFER_SUPPORTED | NDIS_STATISTICS_BYTES_RCV_SUPPORTED | \
    NDIS_STATISTICS_BYTES_XMIT_SUPPORTED | NDIS_STATISTICS_GEN_STATISTICS_SUPPORTED | \
    NDIS_STATISTICS_DIRECTED_FRAMES_XMIT_SUPPORTED | NDIS_STATISTICS_MULTICAST_FRAMES_XMIT_SUPPORTED | \
    NDIS_STATISTICS_BROADCAST_FRAMES_XMIT_SUPPORTED | NDIS_STATISTICS_DIRECTED_FRAMES_RCV_SUPPORTED | \
    NDIS_STATISTICS_MULTICAST_FRAMES_RCV_SUPPORTED | NDIS_STATISTICS_BROADCAST_FRAMES_RCV_SUPPORTED | \
    NDIS_STATISTICS_DIRECTED_BYTES_XMIT_SUPPORTED | NDIS_STATISTICS_MULTICAST_BYTES_XMIT_SUPPORTED | \
    NDIS_STATISTICS_BROADCAST_BYTES_XMIT_SUPPORTED | NDIS_STATISTICS_DIRECTED_BYTES_RCV_SUPPORTED | \
    NDIS_STATISTICS_MULTICAST_BYTES_RCV_SUPPORTED | NDIS_STATISTICS_BROADCAST_BYTES_RCV_SUPPORTED | \
    NDIS_STATISTICS_RCV_DISCARDS_SUPPORTED | NDIS_STATISTICS_XMIT_DISCARDS_SUPPORTED)

typedef struct TX_SLOT {
    PNET_BUFFER_LIST nbl;
    ULONG length, kind;
    BOOLEAN last;
    ULONGLONG started;
} TX_SLOT;

typedef struct RX_SLOT {
    PMDL mdl;
    PNET_BUFFER_LIST nbl;
    UCHAR data[GEM_BUFFER_SIZE];
} RX_SLOT;

typedef struct ADAPTER {
    NDIS_HANDLE handle, configuration, nblPool, interrupt;
    HANDLE interruptRoute;
    PUCHAR registers;
    PHYSICAL_ADDRESS mmioAddress, dmaAddress;
    PDMA_ADAPTER dmaAdapter;
    volatile UCHAR *dma;
    GEM_IO io;
    KSPIN_LOCK lock;
    /* PASSIVE_LEVEL worker/lifecycle exclusion, separate from the send lock. */
    KMUTEX lifecycleMutex;
    KEVENT wake, dpcIdle;
    HANDLE thread;
    volatile LONG terminate, surpriseRemoved;
    BOOLEAN running, powered, fault, dmaTouched, retainDma, ownsHardware, probeOnly;
    BOOLEAN dpcActive;
    /* Only the ISR and NdisMSynchronizeWithInterruptEx callbacks access these. */
    BOOLEAN irqRunning, irqDeferred, irqSuppressed, irqClearOnRead;
    ULONG irqPending;
    volatile LONG recoveryRequested;
    volatile LONG64 interrupts, interruptDpcs, rxInterrupts, txInterrupts;
    ULONG recoveries, lastRecovery;
    ULONG speed, packetFilter, lookahead, multicastCount;
    UCHAR permanent[6], address[6], multicast[MULTICAST_MAX][6];
    ULONG txHead, txTail, txCount, rxHead;
    TX_SLOT tx[GEM_RING_SIZE];
    RX_SLOT rx[RX_BATCH_SIZE];
    UCHAR txCopy[GEM_BUFFER_SIZE];
    ULONGLONG txFrames[3], rxFrames[3], txBytes, rxBytes;
    ULONGLONG txKindBytes[3], rxKindBytes[3];
    ULONGLONG txErrors, rxErrors, rxDiscards, txDiscards, rxNoBuffer;
    ULONGLONG rxAlignment, txOneCollision, txMoreCollisions;
    ULONGLONG rxResource, rxOverrun, rxFiltered;
} ADAPTER;

static NDIS_HANDLE DriverHandle;
static NDIS_OID Oids[] = {
    OID_GEN_SUPPORTED_LIST, OID_GEN_HARDWARE_STATUS, OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE, OID_GEN_PHYSICAL_MEDIUM, OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE, OID_GEN_MAXIMUM_TOTAL_SIZE, OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE, OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE, OID_GEN_RECEIVE_BLOCK_SIZE, OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION, OID_GEN_CURRENT_PACKET_FILTER, OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_DRIVER_VERSION, OID_GEN_VENDOR_DRIVER_VERSION, OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS, OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_XMIT_OK, OID_GEN_RCV_OK, OID_GEN_XMIT_ERROR, OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER, OID_GEN_STATISTICS, OID_GEN_INTERRUPT_MODERATION,
    OID_GEN_LINK_PARAMETERS, OID_802_3_PERMANENT_ADDRESS, OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST, OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT, OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS, OID_PNP_CAPABILITIES,
    OID_PNP_QUERY_POWER, OID_PNP_SET_POWER
};

DRIVER_INITIALIZE DriverEntry;
MINIPORT_INITIALIZE Initialize;
MINIPORT_HALT Halt;
MINIPORT_UNLOAD Unload;
MINIPORT_PAUSE Pause;
MINIPORT_RESTART Restart;
MINIPORT_OID_REQUEST OidRequest;
MINIPORT_SEND_NET_BUFFER_LISTS Send;
MINIPORT_RETURN_NET_BUFFER_LISTS Return;
MINIPORT_CANCEL_SEND CancelSend;
MINIPORT_DEVICE_PNP_EVENT_NOTIFY PnpEvent;
MINIPORT_SHUTDOWN Shutdown;
MINIPORT_CANCEL_OID_REQUEST CancelOid;
static KSTART_ROUTINE MaintenanceThread;
static MINIPORT_ISR Interrupt;
static MINIPORT_INTERRUPT_DPC InterruptDpc;
static MINIPORT_DISABLE_INTERRUPT DisableInterrupt;
static MINIPORT_ENABLE_INTERRUPT EnableInterrupt;
static MINIPORT_SYNCHRONIZE_INTERRUPT StopInterrupts;
static MINIPORT_SYNCHRONIZE_INTERRUPT StartInterrupts;
static MINIPORT_SYNCHRONIZE_INTERRUPT RearmInterrupts;
static MINIPORT_SYNCHRONIZE_INTERRUPT TakeInterrupts;
static MINIPORT_SYNCHRONIZE_INTERRUPT RemoveInterrupts;

static uint32_t ReadRegister(void *context, uint32_t offset)
{
    ADAPTER *a = context;
    return READ_REGISTER_ULONG((PULONG)(a->registers + offset));
}

static void WriteRegister(void *context, uint32_t offset, uint32_t value)
{
    ADAPTER *a = context;
    WRITE_REGISTER_ULONG((PULONG)(a->registers + offset), value);
    /* RP1 is behind PCIe: drain each posted register write. */
    (void)READ_REGISTER_ULONG((PULONG)(a->registers + GEM_NSR));
}

static void Barrier(void *context)
{ UNREFERENCED_PARAMETER(context); KeMemoryBarrier(); }

static void DelayUs(void *context, unsigned us)
{
    LARGE_INTEGER interval;
    UNREFERENCED_PARAMETER(context);
    if (us <= 50) KeStallExecutionProcessor(us);
    else {
        interval.QuadPart = -(LONGLONG)us * 10;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
}

static void LockLifecycle(ADAPTER *a)
{ KeWaitForSingleObject(&a->lifecycleMutex, Executive, KernelMode, FALSE, NULL); }
static void UnlockLifecycle(ADAPTER *a)
{ KeReleaseMutex(&a->lifecycleMutex, FALSE); }

/* The per-file lease is held until halt. The provider owns RP1's router;
   this miniport maps only GEM and never writes USB or PCIe routing registers. */
_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS OpenInterruptRoute(ADAPTER *a)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(RP1_SERVICE_NAME);
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    RP1_IRQ_REQUEST request = { RP1_SERVICE_VERSION, 6 };
    NTSTATUS status;
    InitializeObjectAttributes(&attributes, &name,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = ZwCreateFile(&a->interruptRoute, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
        &attributes, &iosb, NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(status)) { a->interruptRoute = NULL; return status; }
    status = ZwDeviceIoControlFile(a->interruptRoute, NULL, NULL, NULL, &iosb,
        IOCTL_RP1_ACQUIRE_IRQ, &request, sizeof(request), NULL, 0);
    if (status == STATUS_PENDING) {
        ZwWaitForSingleObject(a->interruptRoute, FALSE, NULL);
        status = iosb.Status;
    }
    if (!NT_SUCCESS(status)) { ZwClose(a->interruptRoute); a->interruptRoute = NULL; }
    return status;
}

/* DIRQL: never take the send lock, wait, indicate packets, or do MDIO here. */
_Use_decl_annotations_
static BOOLEAN StopInterrupts(PVOID context)
{
    ADAPTER *a = context;
    a->irqRunning = FALSE;
    a->irqDeferred = FALSE;
    a->irqPending = 0;
    if (!a->surpriseRemoved) WriteRegister(a, GEM_IDR, UINT32_MAX);
    return TRUE;
}

_Use_decl_annotations_
static BOOLEAN StartInterrupts(PVOID context)
{
    ADAPTER *a = context;
    a->irqPending = 0;
    a->irqDeferred = FALSE;
    /* Clear stale status before enabling DMA, so a first packet cannot be lost. */
    (void)gem_interrupt_status(&a->io, a->irqClearOnRead);
    a->irqRunning = TRUE;
    if (!a->irqSuppressed) WriteRegister(a, GEM_IER, GEM_IRQ_MASK);
    return TRUE;
}

_Use_decl_annotations_
static BOOLEAN RearmInterrupts(PVOID context)
{
    ADAPTER *a = context;
    a->irqDeferred = FALSE;
    /* Do not acknowledge here: events arriving while masked must retrigger. */
    if (a->irqRunning && !a->irqSuppressed && !a->surpriseRemoved)
        WriteRegister(a, GEM_IER, GEM_IRQ_MASK);
    return TRUE;
}

typedef struct INTERRUPT_STATUS { ADAPTER *adapter; ULONG status; } INTERRUPT_STATUS;
_Use_decl_annotations_
static BOOLEAN TakeInterrupts(PVOID context)
{
    INTERRUPT_STATUS *s = context;
    s->status = s->adapter->irqPending;
    s->adapter->irqPending = 0;
    if (s->adapter->irqRunning && !s->adapter->surpriseRemoved)
        s->status |= gem_interrupt_status(&s->adapter->io, s->adapter->irqClearOnRead) & GEM_IRQ_MASK;
    return TRUE;
}

_Use_decl_annotations_
static BOOLEAN RemoveInterrupts(PVOID context)
{
    ADAPTER *a = context;
    a->irqRunning = FALSE;
    InterlockedExchange(&a->surpriseRemoved, 1);
    return TRUE; /* The disappeared device must not be accessed. */
}

_Use_decl_annotations_
static VOID DisableInterrupt(NDIS_HANDLE context)
{
    ADAPTER *a = context;
    a->irqSuppressed = TRUE;
    if (!a->surpriseRemoved) WriteRegister(a, GEM_IDR, UINT32_MAX);
}

_Use_decl_annotations_
static VOID EnableInterrupt(NDIS_HANDLE context)
{
    ADAPTER *a = context;
    a->irqSuppressed = FALSE;
    if (a->irqRunning && !a->irqDeferred && !a->surpriseRemoved)
        WriteRegister(a, GEM_IER, GEM_IRQ_MASK);
}

_Use_decl_annotations_
static BOOLEAN Interrupt(NDIS_HANDLE context, PBOOLEAN queueDpc, PULONG targetProcessors)
{
    ADAPTER *a = context;
    ULONG status;
    *queueDpc = FALSE;
    *targetProcessors = 0;
    if (!a->irqRunning || a->irqDeferred || a->irqSuppressed || a->surpriseRemoved) return FALSE;
    status = gem_interrupt_status(&a->io, a->irqClearOnRead) & GEM_IRQ_MASK;
    if (!status) return FALSE; /* Shared GIC line: this interrupt belongs to another client. */
    WriteRegister(a, GEM_IDR, GEM_IRQ_MASK);
    a->irqDeferred = TRUE;
    a->irqPending |= status;
    InterlockedIncrement64(&a->interrupts);
    if (status & GEM_IRQ_RX) InterlockedIncrement64(&a->rxInterrupts);
    if (status & GEM_IRQ_TX) InterlockedIncrement64(&a->txInterrupts);
    *queueDpc = TRUE;
    return TRUE;
}

static void Diagnostic(ADAPTER *a, PCWSTR name, ULONG value)
{
    NDIS_STRING key;
    NDIS_CONFIGURATION_PARAMETER parameter;
    NDIS_CONFIGURATION_OBJECT object = {0};
    NDIS_HANDLE configuration = a->configuration;
    NDIS_STATUS status;
    if (!configuration) {
        object.Header.Type = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
        object.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
        object.Header.Size = sizeof(object);
        object.NdisHandle = a->handle;
        status = NdisOpenConfigurationEx(&object, &configuration);
        if (status != NDIS_STATUS_SUCCESS) return;
    }
    RtlInitUnicodeString(&key, name);
    parameter.ParameterType = NdisParameterInteger;
    parameter.ParameterData.IntegerData = value;
    NdisWriteConfiguration(&status, configuration, &key, &parameter);
    if (status != NDIS_STATUS_SUCCESS)
        DbgPrintEx(DPFLTR_IHVNETWORK_ID, DPFLTR_WARNING_LEVEL,
                   "RP1 Ethernet: diagnostic %wZ write failed: %08lx\n", &key, status);
    if (configuration != a->configuration) NdisCloseConfiguration(configuration);
}

static void Snapshot(ADAPTER *a, ULONG stage)
{
    Diagnostic(a, L"DiagStage", stage);
    if (!a->registers || a->surpriseRemoved) return;
    Diagnostic(a, L"DiagMid", ReadRegister(a, GEM_MID));
    Diagnostic(a, L"DiagNcr", ReadRegister(a, GEM_NCR));
    Diagnostic(a, L"DiagNcfgr", ReadRegister(a, GEM_NCFGR));
    Diagnostic(a, L"DiagNsr", ReadRegister(a, GEM_NSR));
    Diagnostic(a, L"DiagDcfg1", ReadRegister(a, GEM_DCFG1));
    Diagnostic(a, L"DiagDcfg6", ReadRegister(a, GEM_DCFG6));
    Diagnostic(a, L"DiagTsr", ReadRegister(a, GEM_TSR));
    Diagnostic(a, L"DiagRsr", ReadRegister(a, GEM_RSR));
    Diagnostic(a, L"DiagDmaLow", a->dmaAddress.LowPart);
    Diagnostic(a, L"DiagDmaHigh", (ULONG)a->dmaAddress.HighPart);
}

static ULONG FrameKind(const UCHAR *data)
{
    static const UCHAR broadcast[6] = {255,255,255,255,255,255};
    if (RtlEqualMemory(data, broadcast, 6)) return 2;
    return (data[0] & 1) ? 1u : 0u;
}

/* Called under the send lock. */
static BOOLEAN AcceptFrame(ADAPTER *a, const UCHAR *data)
{
    ULONG i, kind = FrameKind(data), filter = a->packetFilter;
    if (filter & NDIS_PACKET_TYPE_PROMISCUOUS) return TRUE;
    if (kind == 2) return !!(filter & NDIS_PACKET_TYPE_BROADCAST);
    if (kind == 0) return (filter & NDIS_PACKET_TYPE_DIRECTED) &&
                          RtlEqualMemory(data, a->address, 6);
    if (filter & NDIS_PACKET_TYPE_ALL_MULTICAST) return TRUE;
    if (filter & NDIS_PACKET_TYPE_MULTICAST)
        for (i = 0; i < a->multicastCount; ++i)
            if (RtlEqualMemory(data, a->multicast[i], 6)) return TRUE;
    return FALSE;
}

static void IndicateLink(ADAPTER *a)
{
    NDIS_LINK_STATE link = {0};
    NDIS_STATUS_INDICATION indication = {0};
    KIRQL irql;
    ULONG speed;
    KeAcquireSpinLock(&a->lock, &irql);
    speed = a->speed;
    KeReleaseSpinLock(&a->lock, irql);
    link.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    link.Header.Revision = NDIS_LINK_STATE_REVISION_1;
    link.Header.Size = NDIS_SIZEOF_LINK_STATE_REVISION_1;
    link.MediaConnectState = speed ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    link.MediaDuplexState = speed ? MediaDuplexStateFull : MediaDuplexStateUnknown;
    link.XmitLinkSpeed = link.RcvLinkSpeed = (ULONG64)speed * 1000000;
    link.PauseFunctions = NdisPauseFunctionsUnsupported;
    link.AutoNegotiationFlags = NDIS_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED |
        NDIS_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED | NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED;
    indication.Header.Type = NDIS_OBJECT_TYPE_STATUS_INDICATION;
    indication.Header.Revision = NDIS_STATUS_INDICATION_REVISION_1;
    indication.Header.Size = NDIS_SIZEOF_STATUS_INDICATION_REVISION_1;
    indication.SourceHandle = a->handle;
    indication.StatusCode = NDIS_STATUS_LINK_STATE;
    indication.StatusBuffer = &link;
    indication.StatusBufferSize = sizeof(link);
    NdisMIndicateStatusEx(a->handle, &indication);
}

/* Stop submission first, then stop DMA before returning any pending sends.
   lifecycleMutex is held. A stuck DMA engine's common buffer is retained at halt. */
static PNET_BUFFER_LIST StopHardware(ADAPTER *a, NDIS_STATUS status)
{
    KIRQL irql;
    PNET_BUFFER_LIST completed = NULL;
    unsigned i;
    KeAcquireSpinLock(&a->lock, &irql);
    a->running = FALSE;
    a->speed = 0;
    InterlockedExchange(&a->recoveryRequested, 0);
    if (a->interrupt) NdisMSynchronizeWithInterruptEx(a->interrupt, 0, StopInterrupts, a);
    if (a->registers && !a->surpriseRemoved) gem_disable(&a->io);
    KeReleaseSpinLock(&a->lock, irql);
    /* A DPC owns its receive NBLs until the synchronous indication returns.
       Queued DPCs observe running=FALSE; active DPCs signal only after completion. */
    KeWaitForSingleObject(&a->dpcIdle, Executive, KernelMode, FALSE, NULL);
    if (a->dmaTouched && !a->surpriseRemoved) {
        for (i = 0; i < 1000; ++i) {
            if (!(ReadRegister(a, GEM_TSR) & GEM_TX_GO)) break;
            DelayUs(a, 10);
        }
        if (i == 1000) a->retainDma = a->fault = TRUE;
        /* Allow outstanding RP1/PCIe transactions to retire after RE/TE=0. */
        DelayUs(a, 10000);
    }
    KeAcquireSpinLock(&a->lock, &irql);
    for (i = 0; i < GEM_RING_SIZE; ++i) {
        if (a->tx[i].nbl && a->tx[i].last) {
            NET_BUFFER_LIST_STATUS(a->tx[i].nbl) = status;
            NET_BUFFER_LIST_NEXT_NBL(a->tx[i].nbl) = completed;
            completed = a->tx[i].nbl;
            a->txDiscards++;
        }
        a->tx[i].nbl = NULL;
    }
    a->txCount = a->txHead = a->txTail = 0;
    KeReleaseSpinLock(&a->lock, irql);
    return completed;
}

static void Complete(ADAPTER *a, PNET_BUFFER_LIST lists, ULONG flags)
{ if (lists) NdisMSendNetBufferListsComplete(a->handle, lists, flags); }

/* lifecycleMutex held; callers have already drained all pending descriptors/DPCs. */
static void StartHardware(ADAPTER *a, ULONG speed)
{
    KIRQL irql;
    gem_init_rings(a->dma, a->dmaAddress.LowPart);
    KeMemoryBarrier();
    gem_configure(&a->io, a->dmaAddress.LowPart, a->address);
    gem_set_speed(&a->io, speed);
    a->rxHead = 0;
    KeAcquireSpinLock(&a->lock, &irql);
    a->speed = speed;
    a->running = TRUE;
    if (speed) {
        a->dmaTouched = TRUE;
        NdisMSynchronizeWithInterruptEx(a->interrupt, 0, StartInterrupts, a);
        WriteRegister(a, GEM_NCR, GEM_MPE | GEM_RE | GEM_TE);
    }
    KeReleaseSpinLock(&a->lock, irql);
}

static void AccumulateErrors(ADAPTER *a)
{
    KIRQL irql;
    ULONG resource, overrun, alignment;
    /* GEM statistics counters clear on read. Exactly one reader accumulates
       hardware drops, including frames which never reached an RX descriptor. */
    KeAcquireSpinLock(&a->lock, &irql);
    if (a->running) {
        alignment = ReadRegister(a, 0x19c);
        resource = ReadRegister(a, 0x1a0);
        overrun = ReadRegister(a, 0x1a4);
        a->rxResource += resource;
        a->rxOverrun += overrun;
        a->rxAlignment += alignment;
        a->rxErrors += alignment + ReadRegister(a, 0x190) + ReadRegister(a, 0x194) +
            ReadRegister(a, 0x198) + ReadRegister(a, 0x184) + ReadRegister(a, 0x188) +
            ReadRegister(a, 0x18c);
        a->rxNoBuffer += (ULONGLONG)resource + overrun;
        a->rxDiscards += (ULONGLONG)resource + overrun;
        a->txOneCollision += ReadRegister(a, 0x138);
        a->txMoreCollisions += ReadRegister(a, 0x13c);
        WriteRegister(a, GEM_RSR, ReadRegister(a, GEM_RSR));
    }
    KeReleaseSpinLock(&a->lock, irql);
}

static void CompleteTransmit(ADAPTER *a)
{
    PNET_BUFFER_LIST completed = NULL;
    GEM_DESC *ring = (GEM_DESC *)(a->dma + GEM_TX_DESC_OFFSET);
    KIRQL irql;
    KeAcquireSpinLock(&a->lock, &irql);
    while (a->running && a->txCount) {
        ULONG control = ring[a->txTail].control;
        TX_SLOT *slot = &a->tx[a->txTail];
        if (!(control & GEM_TX_USED)) break;
        KeMemoryBarrier();
        if (!slot->nbl) {
            /* A published descriptor must retain its NBL until completion. */
            a->running = FALSE;
            NdisMSynchronizeWithInterruptEx(a->interrupt, 0, StopInterrupts, a);
            InterlockedOr(&a->recoveryRequested, 0x40000000);
            KeSetEvent(&a->wake, IO_NO_INCREMENT, FALSE);
            break;
        }
        if (control & GEM_TX_ERRORS) {
            NET_BUFFER_LIST_STATUS(slot->nbl) = NDIS_STATUS_FAILURE;
            a->txErrors++;
        } else {
            a->txFrames[slot->kind]++;
            a->txKindBytes[slot->kind] += slot->length;
            a->txBytes += slot->length;
        }
        if (slot->last) {
            NET_BUFFER_LIST_NEXT_NBL(slot->nbl) = completed;
            completed = slot->nbl;
        }
        slot->nbl = NULL;
        a->txTail = (a->txTail + 1) % GEM_RING_SIZE;
        a->txCount--;
    }
    KeReleaseSpinLock(&a->lock, irql);
    Complete(a, completed, NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL);
}

static ULONG Receive(ADAPTER *a, ULONG limit)
{
    GEM_DESC *ring = (GEM_DESC *)(a->dma + GEM_RX_DESC_OFFSET);
    PNET_BUFFER_LIST first = NULL, last = NULL;
    ULONG n, indicated = 0;
    for (n = 0; n < RX_BATCH_SIZE && indicated < limit; ++n) {
        RX_SLOT *receive = &a->rx[indicated];
        KIRQL irql;
        ULONG address, slot, kind;
        int length;
        BOOLEAN accept = FALSE;
        KeAcquireSpinLock(&a->lock, &irql);
        if (!a->running) { KeReleaseSpinLock(&a->lock, irql); break; }
        slot = a->rxHead;
        address = ring[slot].address;
        if (!(address & GEM_RX_OWN)) { KeReleaseSpinLock(&a->lock, irql); break; }
        KeMemoryBarrier();
        length = gem_rx_length(ring[slot].control);
        if (length > 0) {
            gem_copy_from_dma(receive->data, a->dma + GEM_RX_DATA_OFFSET + slot * GEM_BUFFER_SIZE,
                              (size_t)length);
            accept = AcceptFrame(a, receive->data);
            if (accept) {
                kind = FrameKind(receive->data);
                a->rxFrames[kind]++;
                a->rxKindBytes[kind] += (ULONG)length;
                a->rxBytes += (ULONG)length;
            } else { a->rxDiscards++; a->rxFiltered++; }
        } else a->rxErrors++;
        ring[slot].control = 0;
        KeMemoryBarrier();
        ring[slot].address = address & ~GEM_RX_OWN;
        KeMemoryBarrier();
        a->rxHead = (slot + 1) % GEM_RING_SIZE;
        KeReleaseSpinLock(&a->lock, irql);
        if (accept) {
            NET_BUFFER_DATA_LENGTH(NET_BUFFER_LIST_FIRST_NB(receive->nbl)) = (ULONG)length;
            NET_BUFFER_LIST_NEXT_NBL(receive->nbl) = NULL;
            if (last) NET_BUFFER_LIST_NEXT_NBL(last) = receive->nbl;
            else first = receive->nbl;
            last = receive->nbl;
            indicated++;
        }
    }
    /* Recycle DMA descriptors before entering the stack. RESOURCES keeps
       ownership synchronous, so pause/halt cannot race outstanding receives. */
    if (first) NdisMIndicateReceiveNetBufferLists(a->handle, first, 0, indicated,
        NDIS_RECEIVE_FLAGS_RESOURCES | NDIS_RECEIVE_FLAGS_DISPATCH_LEVEL);
    return indicated;
}

_Use_decl_annotations_
static VOID InterruptDpc(NDIS_HANDLE context, PVOID dpcContext, PVOID throttleContext, PVOID reserved)
{
    ADAPTER *a = context;
    PNDIS_RECEIVE_THROTTLE_PARAMETERS throttle = throttleContext;
    INTERRUPT_STATUS pending = { a, 0 };
    GROUP_AFFINITY target = {0};
    PROCESSOR_NUMBER processor;
    KIRQL irql;
    ULONG received, limit = throttle->MaxNblsToIndicate;
    BOOLEAN moreRx, moreTx;
    GEM_DESC *rx = (GEM_DESC *)(a->dma + GEM_RX_DESC_OFFSET);
    GEM_DESC *tx = (GEM_DESC *)(a->dma + GEM_TX_DESC_OFFSET);
    UNREFERENCED_PARAMETER(dpcContext);
    UNREFERENCED_PARAMETER(reserved);
    throttle->MoreNblsPending = FALSE;
    KeAcquireSpinLock(&a->lock, &irql);
    if (!a->running || !a->speed || a->surpriseRemoved || a->dpcActive) {
        KeReleaseSpinLock(&a->lock, irql);
        return;
    }
    a->dpcActive = TRUE;
    KeClearEvent(&a->dpcIdle);
    NdisMSynchronizeWithInterruptEx(a->interrupt, 0, TakeInterrupts, &pending);
    InterlockedIncrement64(&a->interruptDpcs);
    if (pending.status & GEM_IRQ_FATAL) {
        a->running = FALSE;
        NdisMSynchronizeWithInterruptEx(a->interrupt, 0, StopInterrupts, a);
        InterlockedOr(&a->recoveryRequested, (LONG)(pending.status & GEM_IRQ_FATAL));
        KeSetEvent(&a->wake, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&a->lock, irql);

    CompleteTransmit(a);
    received = Receive(a, limit);

    KeAcquireSpinLock(&a->lock, &irql);
    if (a->running && !a->surpriseRemoved) {
        /* Check ownership after processing, not just the ISR's snapshot. Events
           remain latched while masked, closing the final drain/rearm race. */
        KeMemoryBarrier();
        moreRx = !!(rx[a->rxHead].address & GEM_RX_OWN);
        moreTx = a->txCount && !!(tx[a->txTail].control & GEM_TX_USED);
        if (moreRx && limit != NDIS_INDICATE_ALL_NBLS && received == limit) {
            throttle->MoreNblsPending = TRUE; /* NDIS schedules the continuation. */
        } else if (moreRx || moreTx) {
            /* Stay masked and yield after at most 64 RX descriptors. Queue on
               this CPU so this continuation cannot run before we return. */
            KeGetCurrentProcessorNumberEx(&processor);
            target.Group = processor.Group;
            target.Mask = (KAFFINITY)1 << processor.Number;
            (void)NdisMQueueDpcEx(a->interrupt, 0, &target, NULL);
        } else NdisMSynchronizeWithInterruptEx(a->interrupt, 0, RearmInterrupts, a);
    }
    a->dpcActive = FALSE;
    KeSetEvent(&a->dpcIdle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&a->lock, irql);
}

_Use_decl_annotations_
static void MaintenanceThread(PVOID context)
{
    ADAPTER *a = context;
    LARGE_INTEGER interval;
    ULONGLONG nextStatistics = 0;
    interval.QuadPart = -10000000; /* PHY/health only; never services packet rings. */
    while (!a->terminate) {
        ULONG recovery;
        KIRQL irql;
        KeWaitForSingleObject(&a->wake, Executive, KernelMode, FALSE, &interval);
        if (a->terminate) break;
        LockLifecycle(a);
        recovery = (ULONG)InterlockedExchange(&a->recoveryRequested, 0);
        KeAcquireSpinLock(&a->lock, &irql);
        if (a->running && a->txCount &&
            KeQueryInterruptTime() - a->tx[a->txTail].started > 50000000)
            recovery |= 0x80000000u; /* Hung TX: fail and reset, never poll-complete. */
        KeReleaseSpinLock(&a->lock, irql);
        if (recovery && a->powered && !a->surpriseRemoved && !a->probeOnly) {
            ULONG speed = a->speed;
            PNET_BUFFER_LIST cancelled = StopHardware(a, NDIS_STATUS_FAILURE);
            a->recoveries++;
            a->lastRecovery = recovery;
            Complete(a, cancelled, 0);
            if (!a->fault) StartHardware(a, speed);
            Snapshot(a, 90);
            IndicateLink(a);
        }
        if (a->running && !a->surpriseRemoved) {
            int speed = gem_link(&a->io);
            if (speed < 0) speed = 0;
            if (a->speed != (ULONG)speed) {
                PNET_BUFFER_LIST cancelled = StopHardware(a, NDIS_STATUS_MEDIA_DISCONNECTED);
                Complete(a, cancelled, 0);
                /* Speed bits change only with MAC RX/TX disabled. */
                if (!a->fault) StartHardware(a, (ULONG)speed);
                IndicateLink(a);
                Diagnostic(a, L"DiagLinkMbps", a->speed);
            }
            AccumulateErrors(a);
            if (KeQueryInterruptTime() >= nextStatistics) {
                ULONGLONG filtered;
                KeAcquireSpinLock(&a->lock, &irql);
                filtered = a->rxFiltered;
                KeReleaseSpinLock(&a->lock, irql);
                Diagnostic(a, L"DiagRxResourceLow", (ULONG)a->rxResource);
                Diagnostic(a, L"DiagRxOverrunLow", (ULONG)a->rxOverrun);
                Diagnostic(a, L"DiagRxFilteredLow", (ULONG)filtered);
                Diagnostic(a, L"DiagInterrupts", (ULONG)InterlockedCompareExchange64(&a->interrupts, 0, 0));
                Diagnostic(a, L"DiagInterruptDpcs", (ULONG)InterlockedCompareExchange64(&a->interruptDpcs, 0, 0));
                Diagnostic(a, L"DiagRxInterrupts", (ULONG)InterlockedCompareExchange64(&a->rxInterrupts, 0, 0));
                Diagnostic(a, L"DiagTxInterrupts", (ULONG)InterlockedCompareExchange64(&a->txInterrupts, 0, 0));
                Diagnostic(a, L"DiagRecoveries", a->recoveries);
                Diagnostic(a, L"DiagLastRecovery", a->lastRecovery);
                Diagnostic(a, L"DiagStatisticsUptimeSeconds", (ULONG)(KeQueryInterruptTime() / 10000000));
                nextStatistics = KeQueryInterruptTime() + 100000000;
            }
        }
        UnlockLifecycle(a);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID Send(NDIS_HANDLE context, PNET_BUFFER_LIST lists, NDIS_PORT_NUMBER port, ULONG flags)
{
    ADAPTER *a = context;
    PNET_BUFFER_LIST nbl, next, completed = NULL;
    KIRQL irql;
    ULONG completionFlags = (flags & NDIS_SEND_FLAGS_DISPATCH_LEVEL) ?
                            NDIS_SEND_COMPLETE_FLAGS_DISPATCH_LEVEL : 0;
    UNREFERENCED_PARAMETER(port);
    KeAcquireSpinLock(&a->lock, &irql);
    for (nbl = lists; nbl; nbl = next) {
        PNET_BUFFER nb;
        ULONG count = 0, index, j;
        NDIS_STATUS status = NDIS_STATUS_SUCCESS;
        GEM_DESC *ring = (GEM_DESC *)(a->dma + GEM_TX_DESC_OFFSET);
        next = NET_BUFFER_LIST_NEXT_NBL(nbl);
        NET_BUFFER_LIST_NEXT_NBL(nbl) = NULL;
        if (!a->running || a->surpriseRemoved) status = NDIS_STATUS_PAUSED;
        else if (!a->speed) status = NDIS_STATUS_MEDIA_DISCONNECTED;
        else {
            for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb; nb = NET_BUFFER_NEXT_NB(nb)) {
                if (++count >= GEM_RING_SIZE || NET_BUFFER_DATA_LENGTH(nb) < 14 ||
                    NET_BUFFER_DATA_LENGTH(nb) > GEM_FRAME_MAX) { status = NDIS_STATUS_INVALID_LENGTH; break; }
            }
            if (!count) status = NDIS_STATUS_INVALID_LENGTH;
            if (count > GEM_RING_SIZE - 1 - a->txCount) status = NDIS_STATUS_RESOURCES;
        }
        index = a->txHead;
        if (status == NDIS_STATUS_SUCCESS) {
            /* Stage the entire NBL while every descriptor remains CPU-owned.
               Mapping failure can never leave a partially published NBL. */
            for (nb = NET_BUFFER_LIST_FIRST_NB(nbl); nb; nb = NET_BUFFER_NEXT_NB(nb)) {
                ULONG length = NET_BUFFER_DATA_LENGTH(nb), padded = length < 60 ? 60 : length;
                PUCHAR data = NdisGetDataBuffer(nb, length, a->txCopy, 1, 0);
                TX_SLOT *slot = &a->tx[index];
                volatile UCHAR *buffer = a->dma + GEM_TX_DATA_OFFSET + index * GEM_BUFFER_SIZE;
                if (!data) { status = NDIS_STATUS_RESOURCES; break; }
                gem_copy_to_dma(buffer, data, length);
                for (j = length; j < padded; ++j) buffer[j] = 0;
                slot->length = padded;
                slot->kind = FrameKind(data);
                index = (index + 1) % GEM_RING_SIZE;
            }
        }
        NET_BUFFER_LIST_STATUS(nbl) = status;
        if (status != NDIS_STATUS_SUCCESS) {
            NET_BUFFER_LIST_NEXT_NBL(nbl) = completed;
            completed = nbl;
            a->txDiscards++;
            continue;
        }
        /* Publish backwards: the first descriptor is the final ownership
           transfer, and one unused descriptor always remains as a stopper. */
        for (j = count; j > 0; --j) {
            ULONG slotIndex = (a->txHead + j - 1) % GEM_RING_SIZE;
            TX_SLOT *slot = &a->tx[slotIndex];
            slot->nbl = nbl;
            slot->last = j == count;
            slot->started = KeQueryInterruptTime();
            KeMemoryBarrier();
            ring[slotIndex].control = slot->length | GEM_TX_LAST |
                                     (slotIndex == GEM_RING_SIZE - 1 ? GEM_TX_WRAP : 0);
        }
        KeMemoryBarrier();
        a->txCount += count;
        a->txHead = index;
        WriteRegister(a, GEM_NCR, GEM_MPE | GEM_RE | GEM_TE | GEM_TSTART);
    }
    KeReleaseSpinLock(&a->lock, irql);
    Complete(a, completed, completionFlags);
}

_Use_decl_annotations_
VOID CancelSend(NDIS_HANDLE context, PVOID cancelId)
{
    ADAPTER *a = context;
    KIRQL irql;
    ULONG i;
    KeAcquireSpinLock(&a->lock, &irql);
    for (i = 0; i < GEM_RING_SIZE; ++i)
        if (a->tx[i].nbl && a->tx[i].last &&
            NDIS_GET_NET_BUFFER_LIST_CANCEL_ID(a->tx[i].nbl) == cancelId)
            NET_BUFFER_LIST_STATUS(a->tx[i].nbl) = NDIS_STATUS_SEND_ABORTED;
    /* Already submitted DMA must finish before its NBL can be returned. */
    KeReleaseSpinLock(&a->lock, irql);
}

_Use_decl_annotations_
VOID Return(NDIS_HANDLE context, PNET_BUFFER_LIST lists, ULONG flags)
{
    UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(lists); UNREFERENCED_PARAMETER(flags);
    NT_ASSERT(FALSE); /* all receives use NDIS_RECEIVE_FLAGS_RESOURCES */
}

_Use_decl_annotations_
NDIS_STATUS Pause(NDIS_HANDLE context, PNDIS_MINIPORT_PAUSE_PARAMETERS parameters)
{
    ADAPTER *a = context;
    PNET_BUFFER_LIST completed;
    UNREFERENCED_PARAMETER(parameters);
    LockLifecycle(a);
    completed = StopHardware(a, NDIS_STATUS_PAUSED);
    UnlockLifecycle(a);
    Complete(a, completed, 0);
    return NDIS_STATUS_SUCCESS;
}

_Use_decl_annotations_
NDIS_STATUS Restart(NDIS_HANDLE context, PNDIS_MINIPORT_RESTART_PARAMETERS parameters)
{
    ADAPTER *a = context;
    UNREFERENCED_PARAMETER(parameters);
    LockLifecycle(a);
    if (a->fault || a->surpriseRemoved || !a->powered) {
        UnlockLifecycle(a); return NDIS_STATUS_HARD_ERRORS;
    }
    if (a->probeOnly) {
        Snapshot(a, 65);
        UnlockLifecycle(a);
        return NDIS_STATUS_SUCCESS;
    }
    StartHardware(a, 0); /* wait for successful PHY negotiation before DMA */
    Snapshot(a, 70);
    UnlockLifecycle(a);
    KeSetEvent(&a->wake, IO_NO_INCREMENT, FALSE);
    return NDIS_STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
static void Cleanup(_In_ __drv_freesMem(Mem)
                    _At_(a->configuration, __drv_freesMem(mem))
                    _At_(a->nblPool, __drv_freesMem(mem)) ADAPTER *a)
{
    ULONG i;
    if (a->thread) {
        InterlockedExchange(&a->terminate, 1);
        KeSetEvent(&a->wake, IO_NO_INCREMENT, FALSE);
        ZwWaitForSingleObject(a->thread, FALSE, NULL);
        ZwClose(a->thread);
    }
    if (a->dmaTouched) Complete(a, StopHardware(a, NDIS_STATUS_CLOSING), 0);
    else if (a->ownsHardware && !a->surpriseRemoved) gem_disable(&a->io);
    /* Disconnect waits for every queued ISR/DPC before any NBL, DMA or MMIO
       storage is freed. Release the provider lease only after GEM is masked. */
    if (a->interrupt) { NdisMDeregisterInterruptEx(a->interrupt); a->interrupt = NULL; }
    if (a->interruptRoute) { ZwClose(a->interruptRoute); a->interruptRoute = NULL; }
    for (i = 0; i < RX_BATCH_SIZE; ++i) {
        PNET_BUFFER_LIST nbl = a->rx[i].nbl;
        PMDL mdl = a->rx[i].mdl;
        a->rx[i].nbl = NULL;
        a->rx[i].mdl = NULL;
        if (nbl) NdisFreeNetBufferList(nbl);
        if (mdl) IoFreeMdl(mdl);
    }
    if (a->nblPool) NdisFreeNetBufferListPool(a->nblPool);
    if (a->dmaAdapter && !a->retainDma) {
        if (a->dma) a->dmaAdapter->DmaOperations->FreeCommonBuffer(a->dmaAdapter,
            GEM_DMA_SIZE, a->dmaAddress, (PVOID)a->dma, FALSE);
        a->dmaAdapter->DmaOperations->PutDmaAdapter(a->dmaAdapter);
    }
    if (a->registers) NdisMUnmapIoSpace(a->handle, a->registers, 0x4000);
    if (a->configuration) NdisCloseConfiguration(a->configuration);
    ExFreePoolWithTag(a, TAG);
}

_Use_decl_annotations_
VOID Halt(NDIS_HANDLE context, NDIS_HALT_ACTION action)
{ UNREFERENCED_PARAMETER(action); Cleanup(context); }

_Use_decl_annotations_
VOID Shutdown(NDIS_HANDLE context, NDIS_SHUTDOWN_ACTION action)
{
    ADAPTER *a = context;
    if (action == NdisShutdownPowerOff && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        PNET_BUFFER_LIST completed;
        /* The maintenance thread is ours, so NDIS cannot stop it for us. */
        InterlockedExchange(&a->terminate, 1);
        KeSetEvent(&a->wake, IO_NO_INCREMENT, FALSE);
        LockLifecycle(a);
        completed = StopHardware(a, NDIS_STATUS_CLOSING);
        UnlockLifecycle(a);
        Complete(a, completed, 0);
        return;
    }
    /* Bugcheck path: no waits, locks or allocations. */
    a->irqRunning = FALSE;
    KeMemoryBarrier();
    if (a->ownsHardware && !a->surpriseRemoved) gem_disable(&a->io);
}

_Use_decl_annotations_
VOID PnpEvent(NDIS_HANDLE context, PNET_DEVICE_PNP_EVENT event)
{
    ADAPTER *a = context;
    if (event->DevicePnPEvent == NdisDevicePnPEventSurpriseRemoved) {
        KIRQL irql;
        LockLifecycle(a);
        KeAcquireSpinLock(&a->lock, &irql);
        if (a->interrupt) NdisMSynchronizeWithInterruptEx(a->interrupt, 0, RemoveInterrupts, a);
        else InterlockedExchange(&a->surpriseRemoved, 1);
        /* RP1 is soldered on; loss of the PCIe parent forbids further MMIO. */
        a->retainDma = TRUE;
        a->running = FALSE;
        KeReleaseSpinLock(&a->lock, irql);
        KeWaitForSingleObject(&a->dpcIdle, Executive, KernelMode, FALSE, NULL);
        UnlockLifecycle(a);
    }
}

_Use_decl_annotations_
VOID CancelOid(NDIS_HANDLE context, PVOID requestId)
{ UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(requestId); }

static void GetStatistics(ADAPTER *a, NDIS_STATISTICS_INFO *s)
{
    RtlZeroMemory(s, sizeof(*s));
    s->Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    s->Header.Revision = NDIS_STATISTICS_INFO_REVISION_1;
    s->Header.Size = NDIS_SIZEOF_STATISTICS_INFO_REVISION_1;
    s->SupportedStatistics = NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_RCV | NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_DIRECTED_FRAMES_XMIT | NDIS_STATISTICS_FLAGS_VALID_MULTICAST_FRAMES_XMIT |
        NDIS_STATISTICS_FLAGS_VALID_BROADCAST_FRAMES_XMIT | NDIS_STATISTICS_FLAGS_VALID_BYTES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_BYTES_XMIT | NDIS_STATISTICS_FLAGS_VALID_RCV_ERROR |
        NDIS_STATISTICS_FLAGS_VALID_XMIT_ERROR | NDIS_STATISTICS_FLAGS_VALID_RCV_DISCARDS |
        NDIS_STATISTICS_FLAGS_VALID_XMIT_DISCARDS | NDIS_STATISTICS_FLAGS_VALID_DIRECTED_BYTES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_MULTICAST_BYTES_RCV | NDIS_STATISTICS_FLAGS_VALID_BROADCAST_BYTES_RCV |
        NDIS_STATISTICS_FLAGS_VALID_DIRECTED_BYTES_XMIT | NDIS_STATISTICS_FLAGS_VALID_MULTICAST_BYTES_XMIT |
        NDIS_STATISTICS_FLAGS_VALID_BROADCAST_BYTES_XMIT;
    s->ifHCInUcastPkts = a->rxFrames[0]; s->ifHCInMulticastPkts = a->rxFrames[1];
    s->ifHCInBroadcastPkts = a->rxFrames[2]; s->ifHCOutUcastPkts = a->txFrames[0];
    s->ifHCOutMulticastPkts = a->txFrames[1]; s->ifHCOutBroadcastPkts = a->txFrames[2];
    s->ifHCInOctets = a->rxBytes; s->ifHCOutOctets = a->txBytes;
    s->ifInErrors = a->rxErrors; s->ifOutErrors = a->txErrors;
    s->ifInDiscards = a->rxDiscards; s->ifOutDiscards = a->txDiscards;
    s->ifHCInUcastOctets = a->rxKindBytes[0]; s->ifHCInMulticastOctets = a->rxKindBytes[1];
    s->ifHCInBroadcastOctets = a->rxKindBytes[2]; s->ifHCOutUcastOctets = a->txKindBytes[0];
    s->ifHCOutMulticastOctets = a->txKindBytes[1]; s->ifHCOutBroadcastOctets = a->txKindBytes[2];
}

_Use_decl_annotations_
NDIS_STATUS OidRequest(NDIS_HANDLE context, PNDIS_OID_REQUEST request)
{
    ADAPTER *a = context;
    NDIS_STATUS status = NDIS_STATUS_SUCCESS;
    KIRQL irql;
    PVOID buffer;
    ULONG length, value = 0, size = sizeof(value);
    ULONGLONG value64 = 0;
    USHORT version = 0x061e;
    PVOID result = &value;
    NDIS_OID oid;
    NDIS_STATISTICS_INFO statistics;
    NDIS_PNP_CAPABILITIES power = {0};
    NDIS_INTERRUPT_MODERATION_PARAMETERS moderation = {0};
    NDIS_LINK_PARAMETERS link = {0};
    static const char vendor[] = "RP1 Ethernet interrupt-driven miniport";
    BOOLEAN set = request->RequestType == NdisRequestSetInformation;
    if (!set && request->RequestType != NdisRequestQueryInformation &&
        request->RequestType != NdisRequestQueryStatistics) return NDIS_STATUS_NOT_SUPPORTED;
    if (set) {
        oid = request->DATA.SET_INFORMATION.Oid;
        buffer = request->DATA.SET_INFORMATION.InformationBuffer;
        length = request->DATA.SET_INFORMATION.InformationBufferLength;
        request->DATA.SET_INFORMATION.BytesRead = request->DATA.SET_INFORMATION.BytesNeeded = 0;
    } else {
        oid = request->DATA.QUERY_INFORMATION.Oid;
        buffer = request->DATA.QUERY_INFORMATION.InformationBuffer;
        length = request->DATA.QUERY_INFORMATION.InformationBufferLength;
        request->DATA.QUERY_INFORMATION.BytesWritten = request->DATA.QUERY_INFORMATION.BytesNeeded = 0;
    }
    KeAcquireSpinLock(&a->lock, &irql);
    if (set) {
        if (oid == OID_GEN_INTERRUPT_MODERATION) {
            status = NDIS_STATUS_INVALID_DATA; /* no configurable hardware moderation */
        } else if (oid == OID_GEN_LINK_PARAMETERS) {
            size = NDIS_SIZEOF_LINK_PARAMETERS_REVISION_1;
            if (length < size) status = NDIS_STATUS_INVALID_LENGTH;
            else {
                RtlCopyMemory(&link, buffer, size);
                if (link.Header.Type != NDIS_OBJECT_TYPE_DEFAULT ||
                    link.Header.Revision != NDIS_LINK_PARAMETERS_REVISION_1 ||
                    link.Header.Size < size) status = NDIS_STATUS_INVALID_DATA;
                /* Only the existing automatic speed/duplex policy is supported.
                   Never report success for a forced speed we did not program. */
                else if ((link.AutoNegotiationFlags & AUTO_LINK) != AUTO_LINK ||
                    link.PauseFunctions != NdisPauseFunctionsUnsupported)
                    status = NDIS_STATUS_NOT_SUPPORTED;
            }
        } else if (oid == OID_802_3_MULTICAST_LIST) {
            if (length % 6) status = NDIS_STATUS_INVALID_LENGTH;
            else if (length > sizeof(a->multicast)) status = NDIS_STATUS_MULTICAST_FULL;
            else { RtlCopyMemory(a->multicast, buffer, length); a->multicastCount = length / 6; size = length; }
        } else if (oid == OID_GEN_CURRENT_PACKET_FILTER || oid == OID_GEN_CURRENT_LOOKAHEAD || oid == OID_PNP_SET_POWER) {
            if (length < sizeof(value)) status = NDIS_STATUS_INVALID_LENGTH;
            else {
                RtlCopyMemory(&value, buffer, sizeof(value));
                if (oid == OID_GEN_CURRENT_PACKET_FILTER) {
                    if (value & ~FILTERS) status = NDIS_STATUS_NOT_SUPPORTED;
                    else a->packetFilter = value;
                } else if (oid == OID_GEN_CURRENT_LOOKAHEAD) {
                    if (value > 1500) status = NDIS_STATUS_INVALID_DATA;
                    else a->lookahead = value;
                } else {
                    /* NO_PAUSE_ON_SUSPEND is deliberately absent: NDIS has
                       drained the worker and DMA through Pause first. */
                    if (value < NdisDeviceStateD0 || value > NdisDeviceStateD3) status = NDIS_STATUS_INVALID_DATA;
                    else if (a->running) status = NDIS_STATUS_NOT_ACCEPTED;
                    else a->powered = value == NdisDeviceStateD0;
                }
            }
        } else status = NDIS_STATUS_NOT_SUPPORTED;
        if (status == NDIS_STATUS_SUCCESS) request->DATA.SET_INFORMATION.BytesRead = size;
        if (status == NDIS_STATUS_INVALID_LENGTH) request->DATA.SET_INFORMATION.BytesNeeded = size;
    } else {
        switch (oid) {
        case OID_GEN_SUPPORTED_LIST: result = Oids; size = sizeof(Oids); break;
        case OID_GEN_HARDWARE_STATUS: value = a->fault ? NdisHardwareStatusNotReady : NdisHardwareStatusReady; break;
        case OID_GEN_MEDIA_SUPPORTED: case OID_GEN_MEDIA_IN_USE: value = NdisMedium802_3; break;
        case OID_GEN_PHYSICAL_MEDIUM: value = NdisPhysicalMedium802_3; break;
        case OID_GEN_MAXIMUM_LOOKAHEAD: case OID_GEN_MAXIMUM_FRAME_SIZE: value = 1500; break;
        case OID_GEN_MAXIMUM_TOTAL_SIZE: value = GEM_FRAME_MAX; break;
        case OID_GEN_LINK_SPEED: value = a->speed * 10000; break;
        case OID_GEN_TRANSMIT_BUFFER_SPACE: case OID_GEN_RECEIVE_BUFFER_SPACE: value = GEM_RING_SIZE * GEM_BUFFER_SIZE; break;
        case OID_GEN_TRANSMIT_BLOCK_SIZE: case OID_GEN_RECEIVE_BLOCK_SIZE: value = GEM_BUFFER_SIZE; break;
        case OID_GEN_VENDOR_ID: value = 0xffffff; break;
        case OID_GEN_VENDOR_DESCRIPTION: result = (PVOID)vendor; size = sizeof(vendor); break;
        case OID_GEN_CURRENT_PACKET_FILTER: value = a->packetFilter; break;
        case OID_GEN_CURRENT_LOOKAHEAD: value = a->lookahead; break;
        case OID_GEN_DRIVER_VERSION: result = &version; size = sizeof(version); break;
        case OID_GEN_VENDOR_DRIVER_VERSION: value = 0x00030000; break;
        case OID_GEN_MAC_OPTIONS: value = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK; break;
        case OID_GEN_MEDIA_CONNECT_STATUS: value = a->speed ? NdisMediaStateConnected : NdisMediaStateDisconnected; break;
        case OID_GEN_MAXIMUM_SEND_PACKETS: value = GEM_RING_SIZE - 1; break;
        case OID_GEN_XMIT_OK: value64 = a->txFrames[0] + a->txFrames[1] + a->txFrames[2]; goto counter;
        case OID_GEN_RCV_OK: value64 = a->rxFrames[0] + a->rxFrames[1] + a->rxFrames[2]; goto counter;
        case OID_GEN_XMIT_ERROR: value64 = a->txErrors; goto counter;
        case OID_GEN_RCV_ERROR: value64 = a->rxErrors; goto counter;
        case OID_GEN_RCV_NO_BUFFER: value64 = a->rxNoBuffer; goto counter;
        case OID_802_3_RCV_ERROR_ALIGNMENT: value64 = a->rxAlignment; goto counter;
        case OID_802_3_XMIT_ONE_COLLISION: value64 = a->txOneCollision; goto counter;
        case OID_802_3_XMIT_MORE_COLLISIONS: value64 = a->txMoreCollisions;
counter:    result = &value64; size = length == sizeof(ULONG) ? sizeof(ULONG) : sizeof(value64); break;
        case OID_GEN_STATISTICS: GetStatistics(a, &statistics); result = &statistics; size = sizeof(statistics); break;
        case OID_802_3_PERMANENT_ADDRESS: result = a->permanent; size = 6; break;
        case OID_802_3_CURRENT_ADDRESS: result = a->address; size = 6; break;
        case OID_802_3_MULTICAST_LIST: result = a->multicast; size = a->multicastCount * 6; break;
        case OID_802_3_MAXIMUM_LIST_SIZE: value = MULTICAST_MAX; break;
        case OID_PNP_CAPABILITIES: result = &power; size = sizeof(power); break;
        case OID_PNP_QUERY_POWER: size = 0; break;
        case OID_GEN_INTERRUPT_MODERATION:
            moderation.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            moderation.Header.Revision = NDIS_INTERRUPT_MODERATION_PARAMETERS_REVISION_1;
            moderation.Header.Size = NDIS_SIZEOF_INTERRUPT_MODERATION_PARAMETERS_REVISION_1;
            moderation.InterruptModeration = NdisInterruptModerationNotSupported;
            result = &moderation; size = sizeof(moderation); break;
        case OID_GEN_LINK_PARAMETERS:
            link.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
            link.Header.Revision = NDIS_LINK_PARAMETERS_REVISION_1;
            link.Header.Size = NDIS_SIZEOF_LINK_PARAMETERS_REVISION_1;
            link.MediaDuplexState = a->speed ? MediaDuplexStateFull : MediaDuplexStateUnknown;
            link.XmitLinkSpeed = link.RcvLinkSpeed = (ULONG64)a->speed * 1000000;
            link.PauseFunctions = NdisPauseFunctionsUnsupported;
            link.AutoNegotiationFlags = AUTO_LINK;
            result = &link; size = NDIS_SIZEOF_LINK_PARAMETERS_REVISION_1; break;
        default: status = NDIS_STATUS_NOT_SUPPORTED; break;
        }
        if (status == NDIS_STATUS_SUCCESS) {
            if (length < size) { request->DATA.QUERY_INFORMATION.BytesNeeded = size; status = NDIS_STATUS_BUFFER_TOO_SHORT; }
            else { if (size) RtlCopyMemory(buffer, result, size); request->DATA.QUERY_INFORMATION.BytesWritten = size; }
        }
    }
    KeReleaseSpinLock(&a->lock, irql);
    return status;
}

_Use_decl_annotations_
NDIS_STATUS Initialize(NDIS_HANDLE handle, NDIS_HANDLE driverContext,
                       PNDIS_MINIPORT_INIT_PARAMETERS parameters)
{
    ADAPTER *a;
    NDIS_STATUS status = NDIS_STATUS_RESOURCES;
    NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES registration = {0};
    NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES general = {0};
    NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS interrupt = {0};
    NDIS_CONFIGURATION_OBJECT configuration = {0};
    NET_BUFFER_LIST_POOL_PARAMETERS pool = {0};
    NDIS_PM_CAPABILITIES power = {0};
    DEVICE_DESCRIPTION description = {0};
    PDEVICE_OBJECT pdo;
    OBJECT_ATTRIBUTES threadAttributes;
    ULONG i, mapRegisters, low, high, phy, interruptCount = 0;
    UINT addressLength = 0;
    PVOID networkAddress = NULL;
    PNDIS_CONFIGURATION_PARAMETER option;
    NDIS_STRING probeKey = RTL_CONSTANT_STRING(L"ProbeOnly");
    uint16_t phyHigh = 0xffff, phyLow = 0xffff;
    NTSTATUS ntstatus;
    UNREFERENCED_PARAMETER(driverContext);
    a = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*a), TAG);
    if (!a) return status;
    a->handle = handle;
    a->powered = TRUE;
    a->probeOnly = TRUE;
    a->lookahead = 1500;
    a->io.context = a; a->io.read = ReadRegister; a->io.write = WriteRegister;
    a->io.delay_us = DelayUs; a->io.barrier = Barrier;
    KeInitializeSpinLock(&a->lock);
    KeInitializeMutex(&a->lifecycleMutex, 0);
    KeInitializeEvent(&a->wake, SynchronizationEvent, FALSE);
    KeInitializeEvent(&a->dpcIdle, NotificationEvent, TRUE);
    registration.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES;
    registration.Header.Revision = NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    registration.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1;
    registration.MiniportAdapterContext = a;
    registration.AttributeFlags = NDIS_MINIPORT_ATTRIBUTES_BUS_MASTER | NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND;
    registration.InterfaceType = NdisInterfaceInternal;
    status = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&registration);
    if (status != NDIS_STATUS_SUCCESS) goto failed;
    configuration.Header.Type = NDIS_OBJECT_TYPE_CONFIGURATION_OBJECT;
    configuration.Header.Revision = NDIS_CONFIGURATION_OBJECT_REVISION_1;
    configuration.Header.Size = sizeof(configuration);
    configuration.NdisHandle = handle;
    status = NdisOpenConfigurationEx(&configuration, &a->configuration);
    if (status != NDIS_STATUS_SUCCESS) goto failed;
    NdisReadConfiguration(&status, &option, a->configuration, &probeKey, NdisParameterInteger);
    if (status == NDIS_STATUS_SUCCESS) a->probeOnly = option->ParameterData.IntegerData != 0;
    Diagnostic(a, L"DiagStage", 10);
    status = NDIS_STATUS_RESOURCE_CONFLICT;
    if (!parameters->AllocatedResources) goto failed;
    for (i = 0; i < parameters->AllocatedResources->Count; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &parameters->AllocatedResources->PartialDescriptors[i];
        if (r->Type == CmResourceTypeInterrupt) {
            if ((r->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED)) ||
                r->ShareDisposition != CmResourceShareShared) {
                status = NDIS_STATUS_RESOURCE_CONFLICT; goto failed;
            }
            ++interruptCount;
        }
        if (r->Type != CmResourceTypeMemory || r->u.Memory.Length != 0x4000) continue;
        if (a->registers) { status = NDIS_STATUS_RESOURCE_CONFLICT; goto failed; }
        a->mmioAddress = r->u.Memory.Start;
        status = NdisMMapIoSpace((PVOID *)&a->registers, handle, a->mmioAddress, 0x4000);
        if (status != NDIS_STATUS_SUCCESS) goto failed;
    }
    if (!a->registers || interruptCount != 1) { status = NDIS_STATUS_RESOURCE_CONFLICT; goto failed; }
    Diagnostic(a, L"DiagMmioLow", a->mmioAddress.LowPart);
    Diagnostic(a, L"DiagMmioHigh", (ULONG)a->mmioAddress.HighPart);
    Snapshot(a, 20);
    status = NDIS_STATUS_ADAPTER_NOT_FOUND;
    if (ReadRegister(a, GEM_MID) != 0x00070109) goto failed;
    /* Never take over an already active DMA engine. */
    if (ReadRegister(a, GEM_NCR) & (GEM_RE | GEM_TE)) goto failed;
    a->ownsHardware = TRUE;
    gem_disable(&a->io);
    a->irqClearOnRead = !!(ReadRegister(a, GEM_DCFG1) & GEM_IRQ_COR);
    WriteRegister(a, GEM_NCR, GEM_MPE | (1u << 5)); /* reset statistics */
    WriteRegister(a, GEM_NCFGR, (ReadRegister(a, GEM_NCFGR) & ~(7u << 18)) | (5u << 18));
    WriteRegister(a, GEM_NCR, GEM_MPE);
    if (!gem_mdio_read(&a->io, 2, &phyHigh) || !gem_mdio_read(&a->io, 3, &phyLow)) goto failed;
    phy = ((ULONG)phyHigh << 16) | phyLow;
    Diagnostic(a, L"DiagPhyId", phy);
    Snapshot(a, 30);
    if (phy != 0x600d84a2) goto failed;
    low = ReadRegister(a, GEM_SA1B); high = ReadRegister(a, GEM_SA1T);
    for (i = 0; i < 4; ++i) a->permanent[i] = (UCHAR)(low >> (i * 8));
    a->permanent[4] = (UCHAR)high; a->permanent[5] = (UCHAR)(high >> 8);
    RtlCopyMemory(a->address, a->permanent, 6);
    NdisReadNetworkAddress(&status, &networkAddress, &addressLength, a->configuration);
    if (status == NDIS_STATUS_SUCCESS && addressLength == 6 && gem_valid_mac(networkAddress))
        RtlCopyMemory(a->address, networkAddress, 6);
    status = NDIS_STATUS_INVALID_ADDRESS;
    if (!gem_valid_mac(a->address)) goto failed;
    if (!gem_valid_mac(a->permanent)) RtlCopyMemory(a->permanent, a->address, 6);
    Snapshot(a, 40);
    if (!gem_phy_setup(&a->io)) { status = NDIS_STATUS_HARD_ERRORS; goto failed; }
    NdisMGetDeviceProperty(handle, &pdo, NULL, NULL, NULL, NULL);
    description.Version = DEVICE_DESCRIPTION_VERSION3;
    description.Master = TRUE;
    description.ScatterGather = TRUE;
    description.InterfaceType = Internal;
    description.DmaAddressWidth = 32;
    description.MaximumLength = GEM_DMA_SIZE;
    a->dmaAdapter = IoGetDmaAdapter(pdo, &description, &mapRegisters);
    status = NDIS_STATUS_RESOURCES;
    if (!a->dmaAdapter) goto failed;
    Diagnostic(a, L"DiagMapRegisters", mapRegisters);
    /* HAL observes the ACPI node's _CCA=0. Never substitute a guessed CPU PA. */
    a->dma = a->dmaAdapter->DmaOperations->AllocateCommonBuffer(a->dmaAdapter,
        GEM_DMA_SIZE, &a->dmaAddress, FALSE);
    Snapshot(a, 50);
    if (!a->dma || !gem_dma_range_valid((uint64_t)a->dmaAddress.QuadPart, GEM_DMA_SIZE)) goto failed;
    pool.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    pool.Header.Revision = NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pool.Header.Size = NDIS_SIZEOF_NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1;
    pool.ProtocolId = NDIS_PROTOCOL_ID_DEFAULT;
    pool.fAllocateNetBuffer = TRUE;
    pool.PoolTag = TAG;
    a->nblPool = NdisAllocateNetBufferListPool(handle, &pool);
    if (!a->nblPool) goto failed;
    for (i = 0; i < RX_BATCH_SIZE; ++i) {
        RX_SLOT *receive = &a->rx[i];
        receive->mdl = IoAllocateMdl(receive->data, sizeof(receive->data), FALSE, FALSE, NULL);
        if (!receive->mdl) goto failed;
        MmBuildMdlForNonPagedPool(receive->mdl);
        receive->nbl = NdisAllocateNetBufferAndNetBufferList(a->nblPool, 0, 0, receive->mdl, 0, 0);
        if (!receive->nbl) goto failed;
        receive->nbl->SourceHandle = handle;
    }
    general.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES;
    general.Header.Revision = NDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    general.Header.Size = NDIS_SIZEOF_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES_REVISION_2;
    general.MediaType = NdisMedium802_3;
    general.PhysicalMediumType = NdisPhysicalMedium802_3;
    general.MtuSize = general.LookaheadSize = 1500;
    general.MaxXmitLinkSpeed = general.MaxRcvLinkSpeed = 1000000000;
    general.MediaConnectState = MediaConnectStateDisconnected;
    general.MediaDuplexState = MediaDuplexStateUnknown;
    general.MacOptions = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND | NDIS_MAC_OPTION_NO_LOOPBACK;
    general.SupportedPacketFilters = FILTERS;
    general.MaxMulticastListSize = MULTICAST_MAX;
    general.MacAddressLength = 6;
    RtlCopyMemory(general.PermanentMacAddress, a->permanent, 6);
    RtlCopyMemory(general.CurrentMacAddress, a->address, 6);
    general.AccessType = NET_IF_ACCESS_BROADCAST;
    general.DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    general.ConnectionType = NET_IF_CONNECTION_DEDICATED;
    general.IfType = IF_TYPE_ETHERNET_CSMACD;
    general.IfConnectorPresent = TRUE;
    general.SupportedStatistics = STATS_SUPPORTED;
    general.SupportedPauseFunctions = NdisPauseFunctionsUnsupported;
    general.SupportedOidList = Oids;
    general.SupportedOidListLength = sizeof(Oids);
    general.AutoNegotiationFlags = NDIS_LINK_STATE_XMIT_LINK_SPEED_AUTO_NEGOTIATED |
        NDIS_LINK_STATE_RCV_LINK_SPEED_AUTO_NEGOTIATED | NDIS_LINK_STATE_DUPLEX_AUTO_NEGOTIATED;
    power.Header.Type = NDIS_OBJECT_TYPE_DEFAULT;
    power.Header.Revision = NDIS_PM_CAPABILITIES_REVISION_1;
    power.Header.Size = NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_1;
    general.PowerManagementCapabilitiesEx = &power;
    status = NdisMSetMiniportAttributes(handle, (PNDIS_MINIPORT_ADAPTER_ATTRIBUTES)&general);
    if (status != NDIS_STATUS_SUCCESS) goto failed;
    if (!a->probeOnly) {
        interrupt.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_INTERRUPT;
        interrupt.Header.Revision = NDIS_MINIPORT_INTERRUPT_REVISION_1;
        interrupt.Header.Size = NDIS_SIZEOF_MINIPORT_INTERRUPT_CHARACTERISTICS_REVISION_1;
        interrupt.InterruptHandler = Interrupt;
        interrupt.InterruptDpcHandler = InterruptDpc;
        interrupt.DisableInterruptHandler = DisableInterrupt;
        interrupt.EnableInterruptHandler = EnableInterrupt;
        status = NdisMRegisterInterruptEx(handle, a, &interrupt, &a->interrupt);
        if (status != NDIS_STATUS_SUCCESS) goto failed;
        if (interrupt.InterruptType != NDIS_CONNECT_LINE_BASED) {
            status = NDIS_STATUS_RESOURCE_CONFLICT; goto failed;
        }
        ntstatus = OpenInterruptRoute(a);
        Diagnostic(a, L"DiagInterruptRouteStatus", (ULONG)ntstatus);
        if (!NT_SUCCESS(ntstatus)) { status = (NDIS_STATUS)ntstatus; goto failed; }
        Diagnostic(a, L"DiagInterruptMode", 1);
        Diagnostic(a, L"DiagInterruptClearOnRead", a->irqClearOnRead);
    }
    InitializeObjectAttributes(&threadAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    ntstatus = PsCreateSystemThread(&a->thread, THREAD_ALL_ACCESS, &threadAttributes, NULL, NULL, MaintenanceThread, a);
    if (!NT_SUCCESS(ntstatus)) { status = NDIS_STATUS_RESOURCES; goto failed; }
    Snapshot(a, 60);
    Diagnostic(a, L"DiagStatus", NDIS_STATUS_SUCCESS);
    NdisCloseConfiguration(a->configuration);
    a->configuration = NULL;
    return NDIS_STATUS_SUCCESS;
failed:
    Diagnostic(a, L"DiagStatus", status);
    Diagnostic(a, L"DiagPhyId", ((ULONG)phyHigh << 16) | phyLow);
    Cleanup(a);
    return status;
}

_Use_decl_annotations_
VOID Unload(PDRIVER_OBJECT driver)
{ UNREFERENCED_PARAMETER(driver); NdisMDeregisterMiniportDriver(DriverHandle); }

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS c = {0};
    c.Header.Type = NDIS_OBJECT_TYPE_MINIPORT_DRIVER_CHARACTERISTICS;
    c.Header.Revision = NDIS_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.Header.Size = NDIS_SIZEOF_MINIPORT_DRIVER_CHARACTERISTICS_REVISION_2;
    c.MajorNdisVersion = 6; c.MinorNdisVersion = 30;
    c.MajorDriverVersion = 0; c.MinorDriverVersion = 3;
    c.InitializeHandlerEx = Initialize; c.HaltHandlerEx = Halt; c.UnloadHandler = Unload;
    c.PauseHandler = Pause; c.RestartHandler = Restart; c.OidRequestHandler = OidRequest;
    c.SendNetBufferListsHandler = Send; c.ReturnNetBufferListsHandler = Return;
    c.CancelSendHandler = CancelSend; c.DevicePnPEventNotifyHandler = PnpEvent;
    c.ShutdownHandlerEx = Shutdown; c.CancelOidRequestHandler = CancelOid;
    return NdisMRegisterMiniportDriver(driver, path, NULL, &c, &DriverHandle);
}
