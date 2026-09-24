/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

   IO.c

Abstract:

    This module contains routines that perform read/write IO operations.

Environment:

    Kernel mode only

Revision History:

--*/

#include "driver.h"
#include "IO.tmh"

#pragma warning(disable:4127) // conditional expression is constant

#ifdef ALLOC_PRAGMA
#endif

VOID
ReadSegmentStateSet(
    PUART_READ_CONTEXT _ReadContext,
    UART_READ_STATE    _NewState
    )
/*++

Routine Description:

    This helper centralize the setting of read state. It can be used to detect
    possible incorrect state transition.

Arguments:

    _ReadContext - read context which has existing state
    _NewState - new read state

Return Value:

    none

--*/
{
    UART_READ_STATE OldState = _ReadContext->ReadSegmentState;

    DoTrace(LEVEL_INFO, TFLAG_IO, ("+<<<< -- %s to %s state -- >>>>",
            OldState == GET_PKT_TYPE    ? "Type"    :
            OldState == GET_PKT_HEADER  ? "Header"  :
            OldState == GET_PKT_PAYLOAD ? "Payload" : "Unknown",
            _NewState == GET_PKT_TYPE    ? "Type"    :
            _NewState == GET_PKT_HEADER  ? "Header"  :
            _NewState == GET_PKT_PAYLOAD ? "Payload" : "Unknown" ));

    // Validate the state transition
    switch (_NewState)
    {
        case GET_PKT_TYPE:
            // Intialize the context for a new packet
            _ReadContext->BytesReadNextSegment = 0;
            _ReadContext->H4Packet.Type = 0;
            _ReadContext->BytesToRead4FullPacket = 0;
            RtlZeroMemory(_ReadContext->H4Packet.Packet.Raw, HCI_ACLDATA_HEADER_LEN);
            break;
        case GET_PKT_HEADER:
        case GET_PKT_PAYLOAD:
            // Reset segment count
            _ReadContext->BytesReadNextSegment = 0;
            break;
    }

    _ReadContext->ReadSegmentState = _NewState;
}

                        // Full packet: match to a Request and complete it.
NTSTATUS
ReadH4PacketComplete(
    PFDO_EXTENSION _FdoExtension,
    UCHAR  _Type,
    _In_reads_bytes_(_BufferLength) PUCHAR _Buffer,
    ULONG  _BufferLength
    )
{
    NTSTATUS Status = STATUS_SUCCESS;


    DoTrace(LEVEL_INFO, TFLAG_IO, ("+ReadH4PacketComplete %S Packet Length %d",
        _Type == (UCHAR) HciPacketEvent ? L"Event" : L"AclData", _BufferLength ));

#if DBG
    // Control-plane diagnostics only; never log ACL payloads or link keys.
    if (_Type == HciPacketEvent && _BufferLength >= 2 &&
        (_Buffer[0] == 0x0e || _Buffer[0] == 0x05 ||
         (_Buffer[0] == 0x3e && _BufferLength >= 3 && _Buffer[2] != 2))) {
        UCHAR prefix[20];
        ULONG i;
        for (i = 0; i < RTL_NUMBER_OF(prefix); ++i) prefix[i] = i < _BufferLength ? _Buffer[i] : 0;
        DoTrace(LEVEL_VERBOSE, TFLAG_HCI, ("HCI event len %lu: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            _BufferLength, prefix[0], prefix[1], prefix[2], prefix[3], prefix[4], prefix[5],
            prefix[6], prefix[7], prefix[8], prefix[9], prefix[10], prefix[11], prefix[12],
            prefix[13], prefix[14], prefix[15], prefix[16], prefix[17], prefix[18], prefix[19]));
    }
    // Tracking last completed packet
    RtlCopyMemory(_FdoExtension->LastPacket, _Buffer, _BufferLength);
    _FdoExtension->LastPacketLength = _BufferLength;
#endif

    if (_Type == (UCHAR) HciPacketEvent)
    {
        Status = ReadRequestComplete(_FdoExtension,
                            HciPacketEvent,
                            _BufferLength,
                            _Buffer,
                            _FdoExtension->ReadEventQueue,
                            &_FdoExtension->EventQueueCount,
                            &_FdoExtension->ReadEventList,
                            &_FdoExtension->EventListCount);
    }
    else
    {
        Status = ReadRequestComplete(_FdoExtension,
                            HciPacketAclData,
                            _BufferLength,
                            _Buffer,
                            _FdoExtension->ReadDataQueue,
                           &_FdoExtension->DataQueueCount,
                           &_FdoExtension->ReadDataList,
                           &_FdoExtension->DataListCount);
    }

    DoTrace(LEVEL_INFO, TFLAG_IO, ("-ReadH4PacketComplete %!STATUS!", Status));

    return Status;
}


NTSTATUS
ReadH4PacketReassemble(PUART_READ_CONTEXT c, ULONG Length, PUCHAR Buffer)
{
    while (Length) {
        ULONG header, payload, remaining, count;
        NTSTATUS status;
        if (c->ReadSegmentState == GET_PKT_TYPE) {
            UCHAR type = *Buffer++; --Length;
            if (type != HciPacketEvent && type != HciPacketAclData) return STATUS_DEVICE_PROTOCOL_ERROR;
            c->H4Packet.Type = type;
            ReadSegmentStateSet(c, GET_PKT_HEADER);
        }
        header = c->H4Packet.Type == HciPacketEvent ? 2 : 4;
        if (c->ReadSegmentState == GET_PKT_HEADER) {
            if (c->BytesReadNextSegment > header) return STATUS_DEVICE_PROTOCOL_ERROR;
            remaining = header - c->BytesReadNextSegment;
            count = Length < remaining ? Length : remaining;
            RtlCopyMemory(c->H4Packet.Packet.Raw + c->BytesReadNextSegment, Buffer, count);
            c->BytesReadNextSegment += count; Buffer += count; Length -= count;
            c->BytesToRead4FullPacket = header - c->BytesReadNextSegment;
            if (c->BytesToRead4FullPacket) continue;
            ReadSegmentStateSet(c, GET_PKT_PAYLOAD);
        }
        if (c->ReadSegmentState != GET_PKT_PAYLOAD) return STATUS_DEVICE_PROTOCOL_ERROR;
        payload = header == 2 ? c->H4Packet.Packet.Raw[1] :
            (ULONG)c->H4Packet.Packet.Raw[2] + ((ULONG)c->H4Packet.Packet.Raw[3] << 8);
        if (payload > (header == 2 ? 255u : HCI_MAX_ACL_PAYLOAD_SIZE) ||
            c->BytesReadNextSegment > payload) return STATUS_DEVICE_PROTOCOL_ERROR;
        remaining = payload - c->BytesReadNextSegment;
        count = Length < remaining ? Length : remaining;
        RtlCopyMemory(c->H4Packet.Packet.Raw + header + c->BytesReadNextSegment, Buffer, count);
        c->BytesReadNextSegment += count; Buffer += count; Length -= count;
        c->BytesToRead4FullPacket = payload - c->BytesReadNextSegment;
        if (!c->BytesToRead4FullPacket) {
            status = ReadH4PacketComplete(c->FdoExtension, c->H4Packet.Type, c->H4Packet.Packet.Raw, header + payload);
            ReadSegmentStateSet(c, GET_PKT_TYPE);
            if (!NT_SUCCESS(status)) return status;
        }
    }
    return STATUS_SUCCESS;
}

VOID
ReadH4PacketCompletionRoutine(
    _In_  WDFREQUEST   _Request,
    _In_  WDFIOTARGET  _Target,
    _In_  PWDF_REQUEST_COMPLETION_PARAMS  _Params,
    _In_  WDFCONTEXT  _Context
    )
/*++

Routine Description:

    This is CR function for reading data from device.  It process the data read and
    send down another request unless there is an error or the request is being
    canceled.

Arguments:

    _Request - a caller allocated WDF Request
    _Target - WDF IO Target
    _Params - Completion parameters
    _Context - Context of this request

Return Value:

    none

--*/
{
    NTSTATUS Status;
    PUART_READ_CONTEXT ReadContext;
    PFDO_EXTENSION FdoExtension;
    ULONG BytesRead;
    WDFMEMORY ReadMemory;
    PUCHAR  OutBuffer;
    size_t  OutBufferSize;
    READ_REQUEST_STATE PreviousState;

    UNREFERENCED_PARAMETER(_Request);
    UNREFERENCED_PARAMETER(_Target);

    // Operation result
    Status = _Params->IoStatus.Status;
    BytesRead =  (ULONG) _Params->Parameters.Read.Length;

    ReadContext = (PUART_READ_CONTEXT) _Context;
    ReadContext->Status = Status;

    // Set to REQUEST_COMPLETE if skip REQUEST_PENDING state.
    PreviousState = InterlockedCompareExchange((PLONG)&ReadContext->RequestState,
                                               REQUEST_COMPLETE,
                                               REQUEST_SENT);

    DoTrace(LEVEL_WARNING, TFLAG_DATA, ("+ReadH4PacketCompletionRoutine %!STATUS! %d BytesRead %S)",
            Status, BytesRead, PreviousState == REQUEST_PENDING ? L"Async" : L"*Sync*"));

    FdoExtension = (PFDO_EXTENSION) ReadContext->FdoExtension;

    //
    // The return status can either be
    //      - successful (buffer completely filled),
    //      - timeout (buffer not completed filled prior to interval timeout expired
    //      - cancellation
    //      - failure
    //
    if (NT_SUCCESS(Status) || Status == STATUS_IO_TIMEOUT || Status == STATUS_TIMEOUT) {
        // Continue to process
    }
    else  {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" ReadH4PacketCompletionRoutine failed %!STATUS!", Status));
        if (Status == STATUS_CANCELLED) {
            //
            // Under regualr operational state, IO Target will only cancel a request
            // when it is ready to abort (e.g. device removal).
            //
        }

        goto Exit;
    }

    ReadMemory = _Params->Parameters.Read.Buffer;
    OutBuffer = (PUCHAR) WdfMemoryGetBuffer(ReadMemory, &OutBufferSize);
    if (OutBufferSize < BytesRead) { Status = STATUS_DEVICE_PROTOCOL_ERROR; goto Exit; }
    DoTrace(LEVEL_INFO, TFLAG_IO, (" ReadH4PacketCompletionRoutine %d BytesRead pBuffer %p", BytesRead, OutBuffer));

    //
    // Process a read buffer if there is data
    //
    if (OutBuffer && BytesRead)
    {
        //
        // Process the incoming data to form partial or full H4 packet
        //
        Status = ReadH4PacketReassemble(ReadContext,
                                        BytesRead,
                                        OutBuffer);

        // If data stream error, ignore the packet and start over.
        if (!NT_SUCCESS(Status))
        {
            FdoExtension->OutOfSyncErrorCount++;
            DoTrace(LEVEL_ERROR, TFLAG_IO, (" ====> [%d] 0x%x  <=====",
                    FdoExtension->OutOfSyncErrorCount,
                    *OutBuffer));
            ReadContext->Status = Status;
            goto Exit;
        }
    }
    else
    {
        Status = STATUS_IO_TIMEOUT; ReadContext->Status = Status; goto Exit;
    }


    if (PreviousState == REQUEST_PENDING)
    {
        ULONG BytesToRead;

        //
        // Determine what is the size of the buffer to send down.
        //
        BytesToRead = (ReadContext->ReadSegmentState == GET_PKT_TYPE ? INITIAL_H4_READ_SIZE :
                       ReadContext->BytesToRead4FullPacket ? ReadContext->BytesToRead4FullPacket :
                       sizeof(FdoExtension->ReadBuffer));

        DoTrace(LEVEL_INFO, TFLAG_IO, (" ReadH4Packet(Read Buffer Size %d bytes)", BytesToRead));

        // Issue next read here since this request was complete asychronously
        // i.e. pending first and then this completion routein is invoked.
        ReadH4Packet(ReadContext,
                     FdoExtension->ReadRequest,
                     FdoExtension->ReadMemory,
                     FdoExtension->ReadBuffer,
                     BytesToRead);
    }
    else
    {
        // Fall through and leave this fucntion if this request was completed synchronously;
        // i.e. this function is invoked first and then return to the RequestSent function.
    }

    DoTrace(LEVEL_INFO, TFLAG_IO, ("-CR_ReadReadIO (fall through)"));

    return;

Exit:

    if (!NT_SUCCESS(Status))
    {
        ReadContext->Status = Status;
        FdoExtension->ReadPumpRunning = FALSE;
        if (Status != STATUS_CANCELLED && FdoExtension->DeviceInitialized)
            WdfDeviceSetFailed(FdoExtension->WdfDevice, WdfDeviceFailedNoRestart);
        DoTrace(LEVEL_WARNING, TFLAG_IO, (" Pump has stopped!"));
    }

    DoTrace(LEVEL_INFO, TFLAG_IO, ("-CR_ReadReadIO (error)"));
}

NTSTATUS
ReadH4Packet(
    _In_  PUART_READ_CONTEXT _ReadContext,
    _In_  WDFREQUEST         _WdfRequest,
    _In_  WDFMEMORY          _WdfMemory,
    _Pre_notnull_ _Pre_writable_byte_size_ (_BufferLen) PVOID _Buffer,
    _In_  ULONG              _BufferLen
    )
/*++

Routine Description:

    Initiate the reading of an HCI packet (event or data) by sending down a read request.

Arguments:

    _ReadContext - Context used for reading data from target UART device

Return Value:

    NTSTATUS

--*/
{
    PFDO_EXTENSION   FdoExtension;
    WDF_REQUEST_REUSE_PARAMS RequestReuseParams;
    NTSTATUS Status;
    WDFMEMORY_OFFSET range;
    UNREFERENCED_PARAMETER(_Buffer);

    DoTrace(LEVEL_INFO, TFLAG_IO, ("+ReadH4Packet"));

    FdoExtension = _ReadContext->FdoExtension;

    if (0 == _BufferLen || _BufferLen > sizeof(FdoExtension->ReadBuffer)) {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" ReadH4Packet: _BufferLen cannot be 0"));
        Status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    while (TRUE) {

        DoTrace(LEVEL_INFO, TFLAG_IO, (" ReadH4Packet - <start>"));
        NT_ASSERT(_ReadContext->RequestState != REQUEST_SENT);

        if (!IsDeviceInitialized(FdoExtension)) {
            Status = STATUS_DEVICE_NOT_READY;
            DoTrace(LEVEL_ERROR, TFLAG_IO, (" ReadH4Packet: cannot attach IO %!STATUS!", Status));
            goto Done;
        }

        //
        // Issue a read event request
        //
        WDF_REQUEST_REUSE_PARAMS_INIT(&RequestReuseParams, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
        Status = WdfRequestReuse(_WdfRequest, &RequestReuseParams);
        if (!NT_SUCCESS(Status)) {
            DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfRequestReuse failed %!STATUS!", Status));
            goto Done;
        }

        if (_BufferLen == 0 || _BufferLen > sizeof(FdoExtension->ReadBuffer)) {
            Status = STATUS_DEVICE_PROTOCOL_ERROR; goto Done;
        }
        range.BufferOffset = 0; range.BufferLength = _BufferLen;

        Status = WdfIoTargetFormatRequestForRead(FdoExtension->IoTargetSerial,
                                                 _WdfRequest,
                                                 _WdfMemory,
                                                 &range, NULL);

        if (!NT_SUCCESS(Status)) {
            DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfIoTargetFormatRequestForRead failed %!STATUS!", Status));
            goto Done;
        }

        // Note: This request is sent to UART driver so it cannot be marked cancellable.
        // But it can be canceled by issuing WdfRequestCancelSentRequest().

        WdfRequestSetCompletionRoutine(_WdfRequest,
                                       ReadH4PacketCompletionRoutine,
                                       _ReadContext);

        InterlockedExchange((PLONG)&_ReadContext->RequestState, REQUEST_SENT);

        if (FALSE == WdfRequestSend(_WdfRequest,
                                    FdoExtension->IoTargetSerial,
                                    WDF_NO_SEND_OPTIONS))
        {
            Status = WdfRequestGetStatus(_WdfRequest);
            DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfRequestSend failed %!STATUS!", Status));

            // Not much we can do if cannot send this request; data pump will be stopped!
            goto Done;
        }
        else
        {
            READ_REQUEST_STATE PreviousState;

            // Set to REQUEST_PENDING if it is in the REQUEST_SENT state.
            PreviousState = InterlockedCompareExchange((PLONG) &_ReadContext->RequestState,
                                                       REQUEST_PENDING,
                                                       REQUEST_SENT);

            DoTrace(LEVEL_WARNING, TFLAG_IO, (" WdfRequestSend ReqState: %d -> %d",
                    PreviousState, _ReadContext->RequestState));

            if (PreviousState == REQUEST_SENT)
            {
                // Request is still pending, and will be completed asychronously in the
                // completion routine where it can issue next read.
                Status = STATUS_PENDING;
                break;
            }
            else
            {
                Status = FdoExtension->ReadContext.Status;
                if (NT_SUCCESS(Status))
                {
                    _BufferLen = _ReadContext->ReadSegmentState == GET_PKT_TYPE ? INITIAL_H4_READ_SIZE :
                        _ReadContext->BytesToRead4FullPacket;
                    if (!_BufferLen || _BufferLen > sizeof(FdoExtension->ReadBuffer)) {
                        Status = STATUS_DEVICE_PROTOCOL_ERROR; break;
                    }
                    // Previous request has been complete synchronously in the
                    // completion routine; do next read in this function.
                }
                else
                {
                    // No tolerance for error
                    break;
                }
            }
        }
    }

Done:

    if (!NT_SUCCESS(Status))
    {
        _ReadContext->Status = Status;
        FdoExtension->ReadPumpRunning = FALSE;
    }

    DoTrace(LEVEL_INFO, TFLAG_IO, ("-ReadH4Packet %!STATUS!", Status));

    return Status;
}

__inline
PHCI_PACKET_ENTRY
HLP_CreatePacketEntry(
    _In_ ULONG  _PacketLength,
    _In_reads_bytes_(_PacketLength) PUCHAR _Packet
    )
{
    PHCI_PACKET_ENTRY  PacketEntry = NULL;

    PacketEntry = (PHCI_PACKET_ENTRY)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(HCI_PACKET_ENTRY) + _PacketLength, POOLTAG_CYPRESSBTUART);
    if (PacketEntry != NULL) {
        InitializeListHead(&PacketEntry->DataEntry);
        RtlCopyMemory(PacketEntry->Packet, _Packet, _PacketLength);
        PacketEntry->PacketLen = _PacketLength;
    }

    return PacketEntry;
}


_Use_decl_annotations_
NTSTATUS
ReadRequestComplete(PFDO_EXTENSION c, UCHAR Type, ULONG Length, PUCHAR Packet,
    WDFQUEUE Queue, PLONG QueueCount, PLIST_ENTRY List, PLONG ListCount)
{
    PHCI_PACKET_ENTRY entry = NULL, pending = NULL;
    WDFREQUEST request = NULL;
    PBTHX_HCI_READ_WRITE_CONTEXT output;
    NTSTATUS status;
    size_t available, needed;
    if (Packet) {
        if (!Length || Length > MAX_HCI_ACLDATA_SIZE) return STATUS_DEVICE_PROTOCOL_ERROR;
        pending = HLP_CreatePacketEntry(Length, Packet);
        if (!pending) return STATUS_INSUFFICIENT_RESOURCES;
    }
    WdfSpinLockAcquire(c->QueueAccessLock);
    if (pending) {
        if (*ListCount >= 256) {
            WdfSpinLockRelease(c->QueueAccessLock);
            ExFreePool(pending);
            return STATUS_BUFFER_OVERFLOW;
        }
        InsertTailList(List, &pending->DataEntry); ++*ListCount;
    }
    if (!IsListEmpty(List) && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Queue, &request))) {
        --*QueueCount; --*ListCount;
        entry = CONTAINING_RECORD(RemoveHeadList(List), HCI_PACKET_ENTRY, DataEntry);
    }
    WdfSpinLockRelease(c->QueueAccessLock);
    if (!entry) return STATUS_SUCCESS;
    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*output), (PVOID *)&output, &available);
    needed = FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + (size_t)entry->PacketLen;
    if (NT_SUCCESS(status) && needed > available) status = STATUS_BUFFER_TOO_SMALL;
    if (NT_SUCCESS(status)) {
        output->Type = Type; output->DataLen = entry->PacketLen;
        RtlCopyMemory(output->Data, entry->Packet, entry->PacketLen);
        if (Type == HciPacketEvent) InterlockedIncrement(&c->CntEventCompleted);
        else InterlockedIncrement(&c->CntReadDataCompleted);
    }
    ExFreePool(entry);
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? needed : 0);
    // Ownership transferred to the queue; the caller must never complete it again.
    return STATUS_SUCCESS;
}

VOID ReadPacketsDiscard(PFDO_EXTENSION c)
{
    LIST_ENTRY discard;
    PLIST_ENTRY lists[2] = {&c->ReadEventList, &c->ReadDataList};
    ULONG i;
    InitializeListHead(&discard);
    WdfSpinLockAcquire(c->QueueAccessLock);
    for (i = 0; i < 2; ++i)
        while (!IsListEmpty(lists[i])) {
            PLIST_ENTRY entry = RemoveHeadList(lists[i]);
            InsertTailList(&discard, entry);
        }
    c->EventListCount = c->DataListCount = 0;
    WdfSpinLockRelease(c->QueueAccessLock);
    while (!IsListEmpty(&discard)) {
        PHCI_PACKET_ENTRY entry = CONTAINING_RECORD(RemoveHeadList(&discard), HCI_PACKET_ENTRY, DataEntry);
        ExFreePool(entry);
    }
}

_Use_decl_annotations_
VOID ReadResourcesFree(WDFDEVICE Device)
{
    PFDO_EXTENSION c = FdoGetExtension(Device);
    // The UART target has been stopped, so no completion can refill a list.
    ReadPacketsDiscard(c);
    if (c->ReadRequest) { WdfObjectDelete(c->ReadRequest); c->ReadRequest = NULL; }
    if (c->ReadMemory) { WdfObjectDelete(c->ReadMemory); c->ReadMemory = NULL; }
}


NTSTATUS
ReadResourcesAllocate(
    _In_  WDFDEVICE _Device
)
/*++
Routine Description:

    This helper function allocates resource (queues and lists) for managing read IOs
    Request from upper layer or for data pump with the device.

Arguments:

    _Device - WDF Device object

Return Value:

    NTSTATUS - STATUS_SUCCESS Or STATUS_INSUFFICIENT_RESOURCE

--*/
{
    NTSTATUS  Status;
    PFDO_EXTENSION   FdoExtension;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_OBJECT_ATTRIBUTES ObjAttributes;

    DoTrace(LEVEL_INFO, TFLAG_IO,("+ReadResourcesAllocate"));

    FdoExtension = FdoGetExtension(_Device);
    // Cleanup is valid even if the first queue allocation fails.
    InitializeListHead(&FdoExtension->ReadEventList);
    InitializeListHead(&FdoExtension->ReadDataList);

    // HCI_EVENT
    //  Create WDF Queue for pending Read Event Request(s), and
    //  Initialize a List for pre-fetched Event
    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig,
                             WdfIoQueueDispatchManual);

    Status = WdfIoQueueCreate(_Device,
                              &QueueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &FdoExtension->ReadEventQueue);

    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfIoQueueCreate(Event) %!STATUS!", Status));
        goto Done;
    }


    FdoExtension->EventListCount = 0;
    FdoExtension->EventQueueCount  = 0;

    // HCI_DATA
    //  Create WDF Queue for pending Read Data Request(s), and
    //  Initialize a List for pre-fetched Data
    Status = WdfIoQueueCreate(_Device,
                              &QueueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &FdoExtension->ReadDataQueue);

    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfIoQueueCreate(Data) %!STATUS!", Status));
        goto Done;
    }


    FdoExtension->DataListCount = 0;
    FdoExtension->DataQueueCount = 0;

    // Track request from top and HCI packets from device
    FdoExtension->CntCommandReq         = 0;
    FdoExtension->CntCommandCompleted   = 0;

    FdoExtension->CntEventReq           = 0;
    FdoExtension->CntEventCompleted     = 0;

    FdoExtension->CntWriteDataReq       = 0;
    FdoExtension->CntWriteDataCompleted = 0;

    FdoExtension->CntReadDataReq        = 0;
    FdoExtension->CntReadDataCompleted  = 0;

    // Create a WDF Request
    WDF_OBJECT_ATTRIBUTES_INIT(&ObjAttributes);
    ObjAttributes.ParentObject = _Device;

    Status = WdfRequestCreate(&ObjAttributes,
                              FdoExtension->IoTargetSerial,
                              &FdoExtension->ReadRequest);

    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfRequestCreate(ReadRequest) failed %!STATUS!", Status));
        goto Done;
    }

    // Initialize the ReadContext and its initial ReadSegmentState
    RtlZeroMemory(&FdoExtension->ReadContext, sizeof(UART_READ_CONTEXT));
    FdoExtension->ReadContext.FdoExtension = FdoExtension;
    ReadSegmentStateSet(&FdoExtension->ReadContext, GET_PKT_TYPE);

    Status = WdfMemoryCreatePreallocated(&ObjAttributes,
                                         &FdoExtension->ReadBuffer,
                                         sizeof(FdoExtension->ReadBuffer),
                                         &FdoExtension->ReadMemory);

    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" WdfMemoryCreatePreallocated(ReadMemory) failed %!STATUS!", Status));
        goto Done;
    }

Done:

    DoTrace(LEVEL_INFO, TFLAG_IO,("-ReadResourcesAllocate %!STATUS!", Status));
    if (!NT_SUCCESS(Status))
    {
        ReadResourcesFree(_Device);
    }

    return Status;
}
