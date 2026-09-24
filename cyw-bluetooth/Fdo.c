/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    Fdo.c

Abstract:

    This module contains routines to handle the function driver
    aspect of the bus driver.

Environment:

    kernel mode only

--*/

#include "driver.h"
#include <reshub.h>
#include "fdo.tmh"

#define BTHX_VALID_WRITE_PACKET_TYPE(type) (type == HciPacketCommand || type == HciPacketAclData)
#define BTHX_VALID_READ_PACKET_TYPE(type)  (type == HciPacketEvent   || type == HciPacketAclData)

typedef struct {
    PFDO_EXTENSION Fdo;
    BTHX_HCI_PACKET_TYPE *Output, Type;
    WDFMEMORY Memory;
    ULONG Length;
} BT_WRITE_CONTEXT, *PBT_WRITE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BT_WRITE_CONTEXT, BtWriteContext)
EVT_WDF_REQUEST_COMPLETION_ROUTINE BtWriteComplete;
_Use_decl_annotations_
VOID BtWriteComplete(WDFREQUEST Request, WDFIOTARGET Target,
    PWDF_REQUEST_COMPLETION_PARAMS Params, WDFCONTEXT Context)
{
    PBT_WRITE_CONTEXT c = Context;
    NTSTATUS status = Params->IoStatus.Status;
    UNREFERENCED_PARAMETER(Target);
    if (NT_SUCCESS(status) && Params->IoStatus.Information != c->Length) status = STATUS_DEVICE_PROTOCOL_ERROR;
    if (NT_SUCCESS(status)) {
        *c->Output = c->Type;
        if (c->Type == HciPacketCommand) InterlockedIncrement(&c->Fdo->CntCommandCompleted);
        else InterlockedIncrement(&c->Fdo->CntWriteDataCompleted);
    }
    WdfRequestCompleteWithInformation(Request, status, NT_SUCCESS(status) ? sizeof(*c->Output) : 0);
}

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, FdoCreateOneChildDevice)
#pragma alloc_text (PAGE, FdoRemoveOneChildDevice)
#pragma alloc_text (PAGE, FdoCreateAllChildren)
#pragma alloc_text (PAGE, FdoFindConnectResources)
#pragma alloc_text (PAGE, FdoDevPrepareHardware)
#pragma alloc_text (PAGE, FdoDevReleaseHardware)
#pragma alloc_text (PAGE, FdoDevSelfManagedIoInit)
#pragma alloc_text (PAGE, FdoDevSelfManagedIoCleanup)
#pragma alloc_text (PAGE, FdoDevD0Exit)
#pragma alloc_text (PAGE, HlpInitializeFdoExtension)
#endif

//
// Child device node, PDO(s), could be enumerated statically if number of PDOs are known
// at driver start, or dynamic enuermation mechanism is used.  Both methods are presented
// in this code, but only one can be chosen using the define macro (see sources file).
//
#ifdef DYNAMIC_ENUM

typedef struct _ENABLE_PDO_CONTEXT {
    WDFDEVICE     Fdo;
} ENABLE_PDO_CONTEXT, *PENABLE_PDO_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ENABLE_PDO_CONTEXT, GetEnablePdoWorkItemContext)

// Timeout used to delay dev node enuemeration
ULONG g_WaitToEnablePDO = 20000;  // MSec

VOID
DeviceEnablePDOWorker(
    _In_  WDFWORKITEM  _WorkItem
    )
/*++
Routine Description:

    A work item function to dynamically enuermate a PDO.

Arguments:

    _pWorkItem - work item that contains a context to help carrying out its task

Return Value:
--*/
{
    PENABLE_PDO_CONTEXT Context;
    LARGE_INTEGER    RemoteWakeTimeout;

    NTSTATUS Status = STATUS_SUCCESS;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP, ("+DeviceEnablePDOWorker"));
    Context = GetEnablePdoWorkItemContext(_WorkItem);

    RemoteWakeTimeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(g_WaitToEnablePDO);
    KeDelayExecutionThread(KernelMode, FALSE, &RemoteWakeTimeout);

    DoTrace(LEVEL_INFO, TFLAG_PNP, ("+Complete the wait"));

    Status = FdoCreateOneChildDeviceDynamic(Context->Fdo,
                                            BT_PDO_HARDWARE_IDS,
                                            sizeof(BT_PDO_HARDWARE_IDS)/sizeof(WCHAR),
                                            BLUETOOTH_FUNC_IDS );

    WdfObjectDelete(_WorkItem);

    DoTrace(LEVEL_INFO, TFLAG_POWER, ("-DeviceEnablePDOWorker %!STATUS!", Status));

}

NTSTATUS
FdoEvtDeviceListCreatePdo(
    WDFCHILDLIST DeviceList,
    PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    PWDFDEVICE_INIT ChildInit
    )
/*++

Routine Description:

    Called by the framework in response to Query-Device relation when
    a new PDO for a child device needs to be created.

Arguments:

    DeviceList - Handle to the default WDFCHILDLIST created by the framework as part
                 of FDO.

    IdentificationDescription - Decription of the new child device.

    ChildInit - It's a opaque structure used in collecting device settings
                and passed in as a parameter to CreateDevice.

Return Value:

    NT Status code.

--*/
{
    PPDO_IDENTIFICATION_DESCRIPTION pDesc;

    PAGED_CODE();

    pDesc = CONTAINING_RECORD(IdentificationDescription,
                              PDO_IDENTIFICATION_DESCRIPTION,
                              Header);

    return PdoCreateDynamic(WdfChildListGetDevice(DeviceList),
                            ChildInit,
                            pDesc->HardwareIds,
                            pDesc->SerialNo);
}

NTSTATUS
FdoCreateOneChildDeviceDynamic(
    _In_ WDFDEVICE  _Device,
    _In_ PWCHAR     _HardwareIds,
    _In_ size_t     _CchHardwareIds,
    _In_ ULONG      _SerialNo
    )

/*++

Routine Description:

    The trigger event has been signalled that a new device on the bus has arrived.

    We therefore create a description structure in stack, fill in information about
    the child device and call WdfChildListAddOrUpdateChildDescriptionAsPresent
    to add the device.

--*/

{
    PDO_IDENTIFICATION_DESCRIPTION Description;
    NTSTATUS         Status;

    PAGED_CODE ();

    //
    // Initialize the description with the information about the newly
    // plugged in device.
    //
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&Description.Header,
                                                     sizeof(Description));

    Description.SerialNo = _SerialNo;
    Description.CchHardwareIds = _CchHardwareIds;
    Description.HardwareIds = _HardwareIds;

    //
    // Call the framework to add this child to the childlist. This call
    // will internaly call our DescriptionCompare callback to check
    // whether this device is a new device or existing device. If
    // it's a new device, the framework will call DescriptionDuplicate to create
    // a copy of this description in nonpaged pool.
    // The actual creation of the child device will happen when the framework
    // receives QUERY_DEVICE_RELATION request from the PNP manager in
    // response to InvalidateDeviceRelations call made as part of adding
    // a new child.
    //
    Status = WdfChildListAddOrUpdateChildDescriptionAsPresent(WdfFdoGetDefaultChildList(_Device),
                                                              &Description.Header,
                                                               NULL); // AddressDescription

    if (Status == STATUS_OBJECT_NAME_EXISTS) {
        //
        // The description is already present in the list, the serial number is
        // not unique, return error.
        //
        Status = STATUS_INVALID_PARAMETER;
    }

    return Status;
}

#endif  // ifdef DYNAMIC_ENUM

NTSTATUS
FdoCreateOneChildDevice(
    _In_ WDFDEVICE  _Device,
    _In_ PWSTR      _HardwareIds,
    _In_ ULONG      _SerialNo
    )
/*++

Routine Description:

    Create a new PDO, initialize it, add it to the list of PDOs for this
    FDO bus.

Arguments:

    _Device - WDF device object

    _HardwareIDs - hardware Id for a device

    _SerialNo - Unique ID for a child DO

Returns:

    Status

--*/
{
    NTSTATUS         Status = STATUS_SUCCESS;
    BOOLEAN          IsUnique = TRUE;
    WDFDEVICE        ChildDevice;
    PPDO_EXTENSION   PdoExtension;
    PFDO_EXTENSION   FdoExtension;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP, ("+ FdoCreateOneChildDevice() HWID: %S", _HardwareIds));

    //
    // First make sure that we don't already have another device with the
    // same serial number.
    // Framework creates a collection of all the child devices we have
    // created so far. So acquire the handle to the collection and lock
    // it before walking the item.
    //
    FdoExtension = FdoGetExtension(_Device);
    ChildDevice = NULL;

    //
    // We need an additional lock to synchronize addition because
    // WdfFdoLockStaticChildListForIteration locks against anyone immediately
    // updating the static child list (the changes are put on a queue until the
    // list has been unlocked).  This type of lock does not enforce our concept
    // of unique IDs on the bus (ie SerialNo).
    //
    // Without our additional lock, 2 threads could execute this function, both
    // find that the requested SerialNo is not in the list and attempt to add
    // it.  If that were to occur, 2 PDOs would have the same unique SerialNo,
    // which is incorrect.
    //
    // We must use a passive level lock because you can only call WdfDeviceCreate
    // at PASSIVE_LEVEL.
    //
    WdfWaitLockAcquire(FdoExtension->ChildLock, NULL);
    WdfFdoLockStaticChildListForIteration(_Device);

    while ((ChildDevice = WdfFdoRetrieveNextStaticChild(_Device,
                                                        ChildDevice,
                                                        WdfRetrieveAddedChildren)) != NULL) {
        //
        // WdfFdoRetrieveNextStaticChild returns reported and to be reported
        // children (ie children who have been added but not yet reported to PNP).
        //
        // A surprise removed child will not be returned in this list.
        //
        PdoExtension = PdoGetExtension(ChildDevice);

        //
        // It's okay to plug in another device with the same serial number
        // as long as the previous one is in a surprise-removed state. The
        // previous one would be in that state after the device has been
        // physically removed, if somebody has an handle open to it.
        //
        if (_SerialNo == PdoExtension->SerialNo) {
            IsUnique = FALSE;
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

    if (IsUnique) {
        //
        // Create a new child device.  It is OK to create and add a child while
        // the list locked for enumeration.  The enumeration lock applies only
        // to enumeration, not addition or removal.
        //
        Status = PdoCreate(_Device, _HardwareIds, _SerialNo);
    }

    WdfFdoUnlockStaticChildListFromIteration(_Device);
    WdfWaitLockRelease(FdoExtension->ChildLock);

    DoTrace(LEVEL_INFO, TFLAG_PNP, ("- FdoCreateOneChildDevice() %!STATUS!", Status));

    return Status;
}

NTSTATUS
FdoRemoveOneChildDevice(
    WDFDEVICE   _Device,
    ULONG       _SerialNo
    )
/*++

Routine Description:

    The application has told us a device has departed from the bus.

    We therefore need to flag the PDO as no longer present
    and then tell Plug and Play about it.

Arguments:

    _Device - WDF device object

    _SerialNo - Unique ID for a child DO

Returns:

    Status

--*/

{
    PPDO_EXTENSION  PdoExtension;
    BOOLEAN         Found = FALSE;
    BOOLEAN         PlugOutAll;
    WDFDEVICE       ChildDevice;
    NTSTATUS        Status = STATUS_INVALID_PARAMETER;

    PAGED_CODE();

    PlugOutAll = (0 == _SerialNo) ? TRUE : FALSE;

    ChildDevice = NULL;

    WdfFdoLockStaticChildListForIteration(_Device);

    while ((ChildDevice = WdfFdoRetrieveNextStaticChild(_Device,
                                                        ChildDevice,
                                                        WdfRetrieveAddedChildren)) != NULL) {
        if (PlugOutAll) {

            Status = WdfPdoMarkMissing(ChildDevice);
            if(!NT_SUCCESS(Status)) {
                DoTrace(LEVEL_INFO, TFLAG_PNP, ("WdfPdoMarkMissing failed 0x%x\n", Status));
                break;
            }

            Found = TRUE;
        }
        else {
            PdoExtension = PdoGetExtension(ChildDevice);

            if (_SerialNo == PdoExtension->SerialNo) {

                Status = WdfPdoMarkMissing(ChildDevice);
                if(!NT_SUCCESS(Status)) {
                    DoTrace(LEVEL_INFO, TFLAG_PNP, ("WdfPdoMarkMissing failed 0x%x\n", Status));
                    break;
                }

                Found = TRUE;
                break;
            }
        }
    }

    WdfFdoUnlockStaticChildListFromIteration(_Device);

    if (Found) {
        Status = STATUS_SUCCESS;
    }

    return Status;
}

NTSTATUS
FdoCreateAllChildren(
    _In_  WDFDEVICE _Device
    )
/*++
Routine Description:

    The routine enables you to statically enumerate child device functions
    during start.

Arguments:

    _Device - WDF device object

Returns:

    Status

--*/
{
    NTSTATUS       Status;
    PFDO_EXTENSION FdoExtension;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP, (" + FdoCreateAllChildren"));

    //
    // Bus driver enumerates all child devnode in this function.
    // Vendor Specific: retrieve all statically saved devnode info
    //     HWID, COMPATID, etc.
    //

    //
    // This sample code only enuemrate the Bluetooth function as the only
    // child device.
    //
    Status = FdoCreateOneChildDevice(_Device,
                                     BT_PDO_HARDWARE_IDS,
                                     BLUETOOTH_FUNC_IDS);

    FdoExtension = FdoGetExtension(_Device);
    if (NT_SUCCESS(Status)) {
        FdoExtension->IsRadioEnabled = TRUE;
    }

    return Status;
}


NTSTATUS
HlpInitializeFdoExtension(
    WDFDEVICE _Device
    )
/*++
Routine Description:

    This helper function initialize the device context.

Arguments:

    _Device - WDF Device object

Return Value:

    Status

--*/
{
    PFDO_EXTENSION        FdoExtension;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS              Status;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+HlpInitializeFdoExtension"));

    FdoExtension = FdoGetExtension(_Device);
    FdoExtension->WdfDevice = _Device;

    //
    // Set Bluetooth (PDO) capabilities
    //    MaxAclTransferInSize - is used by the host to notify the Bluetooth controller
    //        in HCI_Host_Buffer_Size command to set the maximum  size of the data portion
    //        of an HCI ACL packet that will be sent from the controller to the host.
    //        BthMini will only send down an HCI read request with this data buffer size.
    //
    FdoExtension->BthXCaps.MaxAclTransferInSize = MAX_HCI_ACLDATA_SIZE;
    FdoExtension->BthXCaps.ScoSupport = ScoSupportHCIBypass;  // Only option
    FdoExtension->BthXCaps.MaxScoChannels = 1;           // Limit to 1 HCIBypass channel
    FdoExtension->BthXCaps.IsDeviceIdleCapable = FALSE;   // Disable Idle to S0 and wake
    FdoExtension->BthXCaps.IsDeviceWakeCapable = FALSE;  // Wake from Sx

    //
    // Preallocate Request
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = _Device;

    Status = WdfRequestCreate(&Attributes, FdoExtension->IoTargetSerial, &FdoExtension->RequestIoctlSync);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfRequestCreate failed %!STATUS!", Status));
        goto Exit;
    }

    FdoExtension->HardwareErrorDetected = FALSE;

    Status = WdfRequestCreate(&Attributes, FdoExtension->IoTargetSerial, &FdoExtension->RequestWaitOnError);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfRequestCreate failed %!STATUS!", Status));
        goto Exit;
    }

    Status = WdfMemoryCreatePreallocated(&Attributes,
                                         &FdoExtension->SerErrorMask,
                                         sizeof(FdoExtension->SerErrorMask),
                                         &FdoExtension->WaitMaskMemory);

    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfMemoryCreatePreallocated failed %!STATUS!", Status));
        goto Exit;
    }

    Status = WdfSpinLockCreate(&Attributes, &FdoExtension->QueueAccessLock);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, (" WdfSpinLockCreate failed %!STATUS!", Status));
        goto Exit;
    }

Exit:

    return Status;

}

VOID
FdoEvtDeviceDisarmWake(
    _In_  WDFDEVICE  _Device
    )
/*++
Routine Description:

    This function is invoked by the framework after the bus driver determines
    that an event has awakened the device, and after the bus driver subsequently
    completes the wait/wake IRP.

    This function perform any hardware operations that are needed to disable
    the device's ability to trigger a wake signal after the power has been lowered.

Arguments:

    _Device - WDF Device object

Return Value:

    VOID

--*/
{
    UNREFERENCED_PARAMETER(_Device);
    DoTrace(LEVEL_INFO, TFLAG_PNP,(" FdoEvtDeviceDisarmWake"));
}

NTSTATUS
FdoEvtDeviceArmWake(
    _In_  WDFDEVICE  _Device
    )
/*++
Routine Description:

    This function is invoked while the device is still in the D0 device power state,
    before the bus driver lowers the device's power state but after the framework
    has sent a wait/wake IRP on behalf of the driver.

Arguments:

    _Device - WDF Device object

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS Status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(_Device);

    DoTrace(LEVEL_INFO, TFLAG_PNP,(" FdoEvtDeviceArmWake"));

    return Status;
}

_Use_decl_annotations_
NTSTATUS
FdoFindConnectResources(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PFDO_EXTENSION c = FdoGetExtension(Device);
    ULONG i, uart = 0, gpio = 0;
    UNREFERENCED_PARAMETER(Raw);
    c->UARTConnectionId.QuadPart = c->GPIOConnectionId.QuadPart = c->I2CConnectionId.QuadPart = 0;
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        LARGE_INTEGER *id;
        if (!r || r->Type != CmResourceTypeConnection) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            r->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_UART) { ++uart; id = &c->UARTConnectionId; }
        else if (r->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
            r->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO) { ++gpio; id = &c->GPIOConnectionId; }
        else return STATUS_DEVICE_CONFIGURATION_ERROR;
        id->LowPart = r->u.Connection.IdLowPart; id->HighPart = r->u.Connection.IdHighPart;
    }
    return uart == 1 && gpio == 1 ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}



NTSTATUS
FdoOpenDevice(WDFDEVICE Device, WDFIOTARGET *Target)
{
    PFDO_EXTENSION c = FdoGetExtension(Device);
    WCHAR path[RESOURCE_HUB_PATH_SIZE / sizeof(WCHAR)];
    UNICODE_STRING name;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_TARGET_OPEN_PARAMS open;
    NTSTATUS status;
    *Target = NULL;
    if (!ValidConnectionID(c->UARTConnectionId)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    DoTrace(LEVEL_INFO, TFLAG_PNP, ("UART connection %08lx%08lx", c->UARTConnectionId.HighPart, c->UARTConnectionId.LowPart));
    RtlInitEmptyUnicodeString(&name, path, sizeof(path));
    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&name, c->UARTConnectionId.LowPart, c->UARTConnectionId.HighPart);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject = Device;
    status = WdfIoTargetCreate(Device, &attributes, Target);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_READ | GENERIC_WRITE);
    status = WdfIoTargetOpen(*Target, &open);
    if (!NT_SUCCESS(status)) { WdfObjectDelete(*Target); *Target = NULL; }
    return status;
}


NTSTATUS
FdoSetIdleSettings(
    _In_  WDFDEVICE _Device,
    _In_  IDLE_CAP_STATE   _IdleCapState
    )
/*++
Routine Description:

    This function defines how device idle (Dx) is support while system is in
    (S0) for the Serial Hci device (not its child node, which is supported
    in the PDO).

    If its Enuemrator is "ROOT" (in the case of using a Bluetooth dev board),
    its Idle support is IdleCannotWakeFromS0.  Its power capabilities are
    limited to D0 and D3; it is basically on or off, and there is no Idle
    while in S0.

    Vendor: If its Enumerator is ACPI, then it might be possible to support
    idle while in S0. This is vendor specific.

Arguments:

    _Device - WDF Device object

    IDLE_CAP_STATE - The idle capability state to enter

Return Value:

    NTSTATUS

--*/
{
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS  IdleSettings;
    NTSTATUS  Status = STATUS_SUCCESS;
    BOOLEAN AssignS0IdleSettings = TRUE;

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+FdoSetIdleSettings"));

    switch (_IdleCapState)
    {
    case IdleCapActiveOnly:

        //
        // By default ACPI supports D0 active, and idle to D3 without remote wake.
        // While in D3, only host (e.g. IO request) can wake the device to D0.
        //
        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings,
                                                   IdleCannotWakeFromS0);

        // Low Dx state to enter after IdleTimeout has expired and Idle is enabled.
        IdleSettings.DxState         = PowerDeviceD3;
        IdleSettings.IdleTimeout     = IdleTimeoutDefaultValue;     // Use default (~5 seconds)
        IdleSettings.IdleTimeoutType = DriverManagedIdleTimeout;    // Driver is in control (typically for out of SoC).

        // Idle to DxState is not initially disable, and do not allow user control to enable it (as this is active only).
        IdleSettings.UserControlOfIdleSettings = IdleDoNotAllowUserControl;
        IdleSettings.Enabled         = WdfFalse;

        // Do not wake from D3 to D0 due to system wake (Sx to S0); ie only host app can wake.
        IdleSettings.PowerUpIdleDeviceOnSystemWake = WdfFalse;
        break;

    case IdleCapCanWake:

        //
        // If it has a child PDO and there is a controller (GPIO) being configured to support wake,
        // this state can be supported.
        //
        // Vendor: in order to support idle in S0 for this ACPI enumerated device, specify that the device
        // can wake in S0.  For example, if it can wake from D2 in S0, this should be set in its device section:
        //
        //    Name(_S0W, 0x2)
        //
        // Additionally, the wake interrupt, e.g. HOST_WAKE, will need to be known by ACPI (instead of exposing
        // it directly to this driver as system resource); so that, ACPI will do the arming and wake on this
        // driver's behalf  with Dx state transition.
        //

        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings,
                                                   IdleCanWakeFromS0);

        // Low Dx state to enter after IdleTimeout has expired and Idle is enabled.
        IdleSettings.DxState         = PowerDeviceD2;
        IdleSettings.IdleTimeout     = 0;                           // May want to enter D2 immediately and invoke arm wake callback.
        IdleSettings.IdleTimeoutType = DriverManagedIdleTimeout;    // Driver is in control (typically for out of SoC).

        // Idle to DxState is initially enable, but allow user control as well (e.g to turn off idle support).
        IdleSettings.UserControlOfIdleSettings = IdleAllowUserControl;
        IdleSettings.Enabled         = WdfTrue;

        //
        // Note: wiil invoke EvtDeviceArmWakeFromS0 callback  before entering DxState;
        // Driver can arm for HOST_WAKE interrrupt in the callback.
        //
        break;

    case IdleCapCanTurnOff:

        //
        // If there is no child PDO (e.g. in Radio off mode), in effect the BT radio can be turned off
        // to enter D3 state.  All unused controllers (e.g. GPIO) can be turned off, also
        // the Bluetooth function block.  While in D3 state, only host can wake the device.
        //
        // Here is one approach to prevent the FDO from entering DxState while its PDO is in Dx and there is no pending IO:
        //
        // The PDO can hold a reference on its parent to prevent the parent from going into DxState. This is done in
        // PrepareHardware with WdfDeviceStopIdle() and releasing that reference
        // in the PDO's ReleaseHardware with WdfDeviceResumeIdle(). This applies to the case when the PDO is disabled.
        // In the resource rebalancing case, the FDO may enter D3 shortly and then resume to D0.
        //

        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings,
                                                   IdleCannotWakeFromS0);

        // Low Dx state to enter after IdleTimeout has expired and Idle is enabled.
        IdleSettings.DxState         = PowerDeviceD3;
        IdleSettings.IdleTimeout     = IdleTimeoutDefaultValue;
        IdleSettings.IdleTimeoutType = DriverManagedIdleTimeout;    // Driver is in control (typically for out of SoC).

        // Idle to DxState is initially enabled, but allow user control as well (e.g. do not turn off).
        IdleSettings.UserControlOfIdleSettings = IdleAllowUserControl;
        IdleSettings.Enabled         = WdfTrue;

         // Do not wake from D3 to D0 due to system wake (Sx to S0); ie only host app can wake.
        IdleSettings.PowerUpIdleDeviceOnSystemWake = WdfFalse;
        break;

    default:
        AssignS0IdleSettings = FALSE;
        break;
    }

    if (AssignS0IdleSettings)
    {
        Status = WdfDeviceAssignS0IdleSettings(_Device,
                                               &IdleSettings);
    }

    DoTrace(LEVEL_INFO, TFLAG_PNP,("-FdoSetIdleSettings %!STATUS!", Status));
    return Status;
}

_Use_decl_annotations_
NTSTATUS
FdoDevPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PFDO_EXTENSION c = FdoGetExtension(Device);
    NTSTATUS status;
    c->WdfDevice = Device;
    BtCheckpoint(c, 10, STATUS_PENDING);
    status = FdoFindConnectResources(Device, Raw, Translated);
    if (!NT_SUCCESS(status)) { BtCheckpoint(c, 10, status); return status; }
    status = FdoOpenDevice(Device, &c->IoTargetSerial);
    if (!NT_SUCCESS(status)) { BtCheckpoint(c, 20, status); return status; }
    status = HlpInitializeFdoExtension(Device);
    if (!NT_SUCCESS(status)) return status;
    status = FdoSetIdleSettings(Device, IdleCapActiveOnly);
    if (!NT_SUCCESS(status)) return status;
    BtCheckpoint(c, 30, STATUS_PENDING);
    status = DeviceEnable(Device, TRUE);
    if (!NT_SUCCESS(status)) { BtCheckpoint(c, 30, status); return status; }
    c->DeviceInitialized = DeviceInitialize(c, c->IoTargetSerial, c->RequestIoctlSync, TRUE);
    if (!c->DeviceInitialized) return c->BtStatus;
    status = FdoCreateAllChildren(Device);
    BtCheckpoint(c, 100, status);
    return status;
}


_Use_decl_annotations_
NTSTATUS
FdoDevReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PFDO_EXTENSION c = FdoGetExtension(Device);
    UNREFERENCED_PARAMETER(Translated);
    c->DeviceInitialized = FALSE;
    if (c->IoTargetSerial) { WdfObjectDelete(c->IoTargetSerial); c->IoTargetSerial = NULL; }
    if (c->IoTargetGPIO) {
        (void)DeviceEnable(Device, FALSE);
        WdfObjectDelete(c->IoTargetGPIO); c->IoTargetGPIO = NULL;
    }
    return STATUS_SUCCESS;
}



NTSTATUS
FdoDevSelfManagedIoInit(
    _In_  WDFDEVICE  _Device
)
/*++
Routine Description:

    This PnP CB function is invoked once and will perform IO related resource allocation
    and start the read pump.

Arguments:

    _Device - WDF Device object

Return Value:

    NTSTATUS

--*/
{
    NTSTATUS       Status;
    PFDO_EXTENSION FdoExtension;

    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+FdoDevSelfManagedIoInit"));

    //
    // Preallocate resources needed to perform read opeations
    //
    Status = ReadResourcesAllocate(_Device);

    if (!NT_SUCCESS(Status)) {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" ReadResourcesAllocate failed %!STATUS!", Status));
        goto Exit;
    }

    // Issue pending IO request to prefetch HCI event and data
    FdoExtension = FdoGetExtension(_Device);
    FdoExtension->ReadContext.RequestState = REQUEST_COMPLETE;

    // Start the read pump
    FdoExtension->ReadPumpRunning = TRUE;
    Status = ReadH4Packet(&FdoExtension->ReadContext,
                           FdoExtension->ReadRequest,
                           FdoExtension->ReadMemory,
                           FdoExtension->ReadBuffer,
                           INITIAL_H4_READ_SIZE);

    if (!NT_SUCCESS(Status)) {
        DoTrace(LEVEL_ERROR, TFLAG_IO, (" ReadH4Packet failed %!STATUS!", Status));
        goto Exit;
    }

Exit:

    return Status;
}

VOID
FdoDevSelfManagedIoCleanup(
    _In_  WDFDEVICE  _Device
    )
/*++
Routine Description:

    This PnP CB function is invoked once and will be used here to free resource
    that was alocated in its corresponding SelfMagedInit fucntion.

Arguments:

    _Device - WDF Device object

Return Value:

    none

--*/
{
    PAGED_CODE();

    DoTrace(LEVEL_INFO, TFLAG_PNP,("+FdoDevSelfManagedIoCleanup"));

    //
    // Cancel and free resources
    //
    ReadResourcesFree(_Device);

    return;
}

_Use_decl_annotations_
NTSTATUS
FdoDevD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous)
{
    PFDO_EXTENSION c = FdoGetExtension(Device);
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Previous);
    c->OutOfSyncErrorCount = 0;
    if (c->DeviceInitialized) return STATUS_SUCCESS;
    status = WdfIoTargetStart(c->IoTargetSerial);
    if (!NT_SUCCESS(status)) return status;
    status = DeviceEnable(Device, TRUE);
    if (!NT_SUCCESS(status)) return status;
    c->DeviceInitialized = DeviceInitialize(c, c->IoTargetSerial, c->RequestIoctlSync, TRUE);
    if (!c->DeviceInitialized) return c->BtStatus;
    if (c->ReadRequest) {
        ReadPacketsDiscard(c);
        RtlZeroMemory(&c->ReadContext, sizeof(c->ReadContext));
        c->ReadContext.FdoExtension = c;
        c->ReadContext.RequestState = REQUEST_COMPLETE;
        ReadSegmentStateSet(&c->ReadContext, GET_PKT_TYPE);
        c->ReadPumpRunning = TRUE;
        return ReadH4Packet(&c->ReadContext, c->ReadRequest,
            c->ReadMemory, c->ReadBuffer, INITIAL_H4_READ_SIZE);
    }
    return STATUS_SUCCESS;
}


_Use_decl_annotations_
NTSTATUS
FdoDevD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    PFDO_EXTENSION c = FdoGetExtension(Device);
    UNREFERENCED_PARAMETER(Target);
    c->DeviceInitialized = FALSE;
    WdfIoTargetStop(c->IoTargetSerial, WdfIoTargetCancelSentIo);
    return c->IoTargetGPIO ? DeviceEnable(Device, FALSE) : STATUS_SUCCESS;
}


NTSTATUS
HCIContextValidate(ULONG Index, PBTHX_HCI_READ_WRITE_CONTEXT c)
{
    ULONG size = c->DataLen;
    UNREFERENCED_PARAMETER(Index);
    switch (c->Type) {
    case HciPacketCommand:
        if (size >= 3 && size <= MAX_HCI_CMD_SIZE && size == (ULONG)c->Data[2] + 3) return STATUS_SUCCESS;
        break;
    case HciPacketEvent:
        if (size >= 2 && size <= MAX_HCI_EVENT_SIZE && size == (ULONG)c->Data[1] + 2) return STATUS_SUCCESS;
        break;
    case HciPacketAclData:
        if (size >= 4 && size <= MAX_HCI_ACLDATA_SIZE &&
            size == (ULONG)c->Data[2] + ((ULONG)c->Data[3] << 8) + 4) return STATUS_SUCCESS;
        break;
    }
    return STATUS_INVALID_PARAMETER;
}



_Use_decl_annotations_
NTSTATUS
FdoWriteDeviceIO(WDFREQUEST Request, WDFDEVICE Device, PFDO_EXTENSION c,
    PBTHX_HCI_READ_WRITE_CONTEXT Hci)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    PBT_WRITE_CONTEXT context;
    PUCHAR packet;
    NTSTATUS status;
    ULONG length;
    WDFMEMORY_OFFSET range;
    UNREFERENCED_PARAMETER(Device);
    if (Hci->DataLen > MAX_HCI_ACLDATA_SIZE) return STATUS_INVALID_PARAMETER;
    length = Hci->DataLen + 1;
    if (!c->DeviceInitialized) return STATUS_DEVICE_NOT_READY;
#if DBG
    if (Hci->Type == HciPacketCommand && Hci->DataLen >= 3 && ((Hci->Data[1] == 0x20 &&
        (Hci->Data[0] == 0x13 || Hci->Data[0] == 0x22 || Hci->Data[0] == 0x24)) ||
        (Hci->Data[1] == 0x0c && (Hci->Data[0] == 0x33 || Hci->Data[0] == 0x6d)))) {
        UCHAR prefix[20];
        ULONG i;
        for (i = 0; i < RTL_NUMBER_OF(prefix); ++i) prefix[i] = i < Hci->DataLen ? Hci->Data[i] : 0;
        DoTrace(LEVEL_VERBOSE, TFLAG_HCI, ("HCI command len %lu: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            Hci->DataLen, prefix[0], prefix[1], prefix[2], prefix[3], prefix[4], prefix[5],
            prefix[6], prefix[7], prefix[8], prefix[9], prefix[10], prefix[11], prefix[12],
            prefix[13], prefix[14], prefix[15], prefix[16], prefix[17], prefix[18], prefix[19]));
    }
#endif
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, BT_WRITE_CONTEXT);
    status = WdfObjectAllocateContext(Request, &attributes, &context);
    if (!NT_SUCCESS(status)) return status;
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*context->Output), (PVOID *)&context->Output, NULL);
    if (!NT_SUCCESS(status)) return status;
    context->Fdo = c; context->Type = (BTHX_HCI_PACKET_TYPE)Hci->Type; context->Length = length;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject = Request;
    if (!length || length > MAX_H4_HCI_PACKET_SIZE) return STATUS_INVALID_PARAMETER;
    status = WdfMemoryCreate(&attributes, NonPagedPoolNx, POOLTAG_CYPRESSBTUART,
        MAX_H4_HCI_PACKET_SIZE, &context->Memory, (PVOID *)&packet);
    if (!NT_SUCCESS(status)) return status;
    packet[0] = (UCHAR)Hci->Type; RtlCopyMemory(packet + 1, Hci->Data, Hci->DataLen);
    range.BufferOffset = 0; range.BufferLength = length;
    status = WdfIoTargetFormatRequestForWrite(c->IoTargetSerial, Request, context->Memory, &range, NULL);
    if (!NT_SUCCESS(status)) return status;
    WdfRequestSetCompletionRoutine(Request, BtWriteComplete, context);
    // Forward the original IRP so cancellation follows the normal WDF target path.
    if (!WdfRequestSend(Request, c->IoTargetSerial, WDF_NO_SEND_OPTIONS)) return WdfRequestGetStatus(Request);
    return STATUS_SUCCESS;
}


VOID
FdoIoQuDeviceControl(
    _In_  WDFQUEUE     _Queue,
    _In_  WDFREQUEST   _Request,
    _In_  size_t       _OutputBufferLength,
    _In_  size_t       _InputBufferLength,
    _In_  ULONG        _IoControlCode
    )
/*++

Routine Description:

    This routine is the dispatch routine for device control requests.

Arguments:

    _Queue - Handle to the framework queue object that is associated
            with the I/O request.
    _Request - Handle to a framework request object.

    _OutputBufferLength - length of the request's output buffer,
                        if an output buffer is available.
    _InputBufferLength - length of the request's input buffer,
                        if an input buffer is available.

    _IoControlCode - the driver-defined or system-defined I/O control code
                    (IOCTL) that is associated with the request.

Return Value:

   VOID

--*/
{
    WDFMEMORY   ReqInMemory = NULL, ReqOutMemory = NULL;
    PVOID   InBuffer = NULL, OutBuffer = NULL;
    size_t  InBufferSize = 0, OutBufferSize = 0;
    PFDO_EXTENSION   FdoExtension;
    NTSTATUS    Status = STATUS_SUCCESS;
    WDFDEVICE   Device;
    BOOLEAN CompleteRequest = FALSE;
    ULONG ControlCode = (_IoControlCode & 0x00003ffc) >> 2;
    BTHX_HCI_PACKET_TYPE PacketType;
    PBTHX_HCI_READ_WRITE_CONTEXT HCIContext;

    DoTrace(LEVEL_INFO, TFLAG_IOCTL,("+IoDeviceControl - InBufLen:%d, OutBufLen:%d",
        (ULONG) _InputBufferLength, (ULONG) _OutputBufferLength));

    Device = WdfIoQueueGetDevice(_Queue);

    FdoExtension = FdoGetExtension(Device);

    if (_InputBufferLength)
    {
        Status = WdfRequestRetrieveInputMemory(_Request, &ReqInMemory);
        if (NT_SUCCESS(Status))
        {
            InBuffer = WdfMemoryGetBuffer(ReqInMemory, &InBufferSize);
        }
    }

    if (_OutputBufferLength)
    {
        Status = WdfRequestRetrieveOutputMemory(_Request, &ReqOutMemory);
        if (NT_SUCCESS(Status))
        {
            OutBuffer = WdfMemoryGetBuffer(ReqOutMemory, &OutBufferSize);
        }
    }

    switch (_IoControlCode)
    {
    case IOCTL_BTHX_WRITE_HCI:
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,(" IOCTL_BTHX_WRITE_HCI ---------->"));
        // Validate input and output parameters
        if (!InBuffer || InBufferSize < sizeof(BTHX_HCI_READ_WRITE_CONTEXT) ||
            !OutBuffer || OutBufferSize != sizeof(BTHX_HCI_PACKET_TYPE))
        {
            Status = STATUS_INVALID_PARAMETER;
            DoTrace(LEVEL_ERROR, TFLAG_IOCTL,(" IOCTL_BTHX_WRITE_HCI %!STATUS!", Status));
            break;
        }

        HCIContext = (PBTHX_HCI_READ_WRITE_CONTEXT) InBuffer;
        DoTrace(LEVEL_INFO, TFLAG_IOCTL, ("Write type %u length %lu header %lu first %02x %02x %02x",
            HCIContext->Type, HCIContext->DataLen, (ULONG)FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data),
            InBufferSize > FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) ? HCIContext->Data[0] : 0,
            InBufferSize > FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + 1 ? HCIContext->Data[1] : 0,
            InBufferSize > FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + 2 ? HCIContext->Data[2] : 0));
        if (HCIContext->DataLen > InBufferSize - FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) ||
            !NT_SUCCESS(HCIContextValidate(0, HCIContext))) {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        PacketType = (BTHX_HCI_PACKET_TYPE) HCIContext->Type;

        if (!BTHX_VALID_WRITE_PACKET_TYPE(PacketType))
        {
            Status = STATUS_INVALID_PARAMETER;
            DoTrace(LEVEL_ERROR, TFLAG_IOCTL,(" Mismach Write HCI packet type and IOCTL %!STATUS!", Status));
            break;
        }

        if (PacketType == HciPacketCommand)
        {
            InterlockedIncrement(&FdoExtension->CntCommandReq);
        }
        else
        {
            InterlockedIncrement(&FdoExtension->CntWriteDataReq);
        }

        Status = FdoWriteDeviceIO(_Request,
                                  Device,
                                  FdoExtension,
                                  HCIContext);
        DoTrace(LEVEL_INFO, TFLAG_IOCTL, ("Forward write status %!STATUS!", Status));
        break;

    case IOCTL_BTHX_READ_HCI:
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,(" IOCTL_BTHX_READ_HCI <----------"));
        // Validate input and output parameters
        if (!InBuffer || InBufferSize != sizeof(BTHX_HCI_PACKET_TYPE) ||
            !OutBuffer || OutBufferSize < sizeof(BTHX_HCI_READ_WRITE_CONTEXT))
        {
            Status = STATUS_INVALID_PARAMETER;
            DoTrace(LEVEL_ERROR, TFLAG_IOCTL,(" IOCTL_BTHX_READ_HCI %!STATUS!", Status));
            break;
        }

        PacketType = *((BTHX_HCI_PACKET_TYPE *) InBuffer);

        if (!BTHX_VALID_READ_PACKET_TYPE(PacketType))
        {
            Status = STATUS_INVALID_PARAMETER;
            DoTrace(LEVEL_ERROR, TFLAG_IOCTL,(" IOCTL_BTHX_READ_HCI %!STATUS!", Status));
            break;
        }

        if (PacketType == HciPacketEvent)
        {
            WdfSpinLockAcquire(FdoExtension->QueueAccessLock);
              // Queue the new request to preserve sequential order
              Status = WdfRequestForwardToIoQueue(_Request, FdoExtension->ReadEventQueue);
              if (NT_SUCCESS(Status))
              {
                  InterlockedIncrement(&FdoExtension->EventQueueCount);
                  InterlockedIncrement(&FdoExtension->CntEventReq);
              }
            WdfSpinLockRelease(FdoExtension->QueueAccessLock);

            if (NT_SUCCESS(Status))
            {
                Status = ReadRequestComplete(FdoExtension,
                                             HciPacketEvent,
                                             0, NULL,
                                             FdoExtension->ReadEventQueue,
                                             &FdoExtension->EventQueueCount,
                                             &FdoExtension->ReadEventList,
                                             &FdoExtension->EventListCount);
            }

        }
        else if (PacketType == HciPacketAclData)
        {
            WdfSpinLockAcquire(FdoExtension->QueueAccessLock);
              // Queue the new request to preserve sequential order
              Status = WdfRequestForwardToIoQueue(_Request, FdoExtension->ReadDataQueue);
              if (NT_SUCCESS(Status))
              {
                  InterlockedIncrement(&FdoExtension->DataQueueCount);
                  InterlockedIncrement(&FdoExtension->CntReadDataReq);
              }
            WdfSpinLockRelease(FdoExtension->QueueAccessLock);

            if (NT_SUCCESS(Status))
            {
                Status = ReadRequestComplete(FdoExtension,
                                             HciPacketAclData,
                                             0, NULL,
                                             FdoExtension->ReadDataQueue,
                                             &FdoExtension->DataQueueCount,
                                             &FdoExtension->ReadDataList,
                                             &FdoExtension->DataListCount);
            }
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
            DoTrace(LEVEL_ERROR, TFLAG_IOCTL,(" IOCTL_BTHX_READ_HCI %!STATUS!", Status));
            break;
        }
        break;

    case IOCTL_BTHX_GET_VERSION:
        CompleteRequest = TRUE;
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,("IOCTL_BTHX_GET_VERSION"));

        if (OutBuffer && OutBufferSize >= sizeof(BTHX_VERSION))
        {
            RtlCopyMemory(OutBuffer, &Microsoft_BTHX_DDI_Version, sizeof(BTHX_VERSION));
            WdfRequestCompleteWithInformation(_Request, Status, sizeof(BTHX_VERSION));
            return;
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_BTHX_SET_VERSION:
        CompleteRequest = TRUE;
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,("IOCTL_BTHX_SET_VERSION"));

        if (InBuffer && InBufferSize >= sizeof(BTHX_VERSION))
        {
            BTHX_VERSION SupportedVersion = *((BTHX_VERSION *)InBuffer);

            DoTrace(LEVEL_INFO, TFLAG_IOCTL,("IOCTL_BTHX_SET_VERSION 0x%x", SupportedVersion.Version));

            WdfRequestComplete(_Request, Status);
            return;
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_BTHX_QUERY_CAPABILITIES:
        CompleteRequest = TRUE;
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,("IOCTL_BTHX_QUERY_CAPABILITIES"));

        if (OutBuffer && OutBufferSize >= sizeof(BTHX_CAPABILITIES))
        {
            BTHX_CAPABILITIES *pCaps = (BTHX_CAPABILITIES *) OutBuffer;

            RtlCopyMemory(pCaps, &FdoExtension->BthXCaps, sizeof(BTHX_CAPABILITIES));
            WdfRequestCompleteWithInformation(_Request, Status, sizeof(BTHX_CAPABILITIES));
            return;
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
        }
        break;

    //
    // This IOCTL is used to support radio on/off feature by doing the following
    //    1. Power up/down the Bluetooth radio function, and
    //    2. Add/remove a PDO for Bluetooth devnode;
    //
    case IOCTL_BUSENUM_SET_RADIO_ONOFF_VENDOR_SPECFIC:
        CompleteRequest = TRUE;
        Status = STATUS_NOT_SUPPORTED;
        break;

    default:
        DoTrace(LEVEL_INFO, TFLAG_IOCTL,(" IOCTL_(0x%x, Func %d)", _IoControlCode, ControlCode));
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    if (!NT_SUCCESS(Status) || CompleteRequest)
    {
        WdfRequestComplete(_Request, Status);
    }

    return;
}
