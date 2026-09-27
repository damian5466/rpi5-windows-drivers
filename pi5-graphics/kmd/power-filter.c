/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include "pi5-power-relations.h"

typedef struct {
    WDFWAITLOCK Lock;
    WDFIOTARGET Targets[2];
} POWER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(POWER_CONTEXT, PowerContext)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD PowerAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PowerPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE PowerRelease;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS PowerQueryRelations;
EVT_WDF_IO_TARGET_QUERY_REMOVE PowerQueryRemove;

_Use_decl_annotations_
NTSTATUS PowerQueryRemove(WDFIOTARGET target)
{
    UNREFERENCED_PARAMETER(target);
    return STATUS_DEVICE_BUSY; /* Stop the display before removing its providers. */
}

_Use_decl_annotations_
NTSTATUS PowerQueryRelations(WDFDEVICE device, PIRP irp)
{
    POWER_CONTEXT *c = PowerContext(device);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status;
    if (stack->Parameters.QueryDeviceRelations.Type == PowerRelations) {
        WdfWaitLockAcquire(c->Lock, NULL);
        status = Pi5AppendPowerRelations(irp, c->Targets, RTL_NUMBER_OF(c->Targets));
        WdfWaitLockRelease(c->Lock);
        if (!NT_SUCCESS(status)) {
            irp->IoStatus.Status = status;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return status;
        }
    }
    IoSkipCurrentIrpStackLocation(irp);
    return WdfDeviceWdmDispatchPreprocessedIrp(device, irp);
}

_Use_decl_annotations_
NTSTATUS PowerPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated)
{
    static const PCWSTR names[] = { L"\\Device\\Pi5V3d", L"\\Device\\Pi5Fclk" };
    POWER_CONTEXT *c = PowerContext(device);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;
    UNREFERENCED_PARAMETER(raw); UNREFERENCED_PARAMETER(translated);
    WdfWaitLockAcquire(c->Lock, NULL);
    for (i = 0; i < RTL_NUMBER_OF(names); ++i) {
        WDF_OBJECT_ATTRIBUTES a;
        WDF_IO_TARGET_OPEN_PARAMS p;
        UNICODE_STRING name;
        WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = device;
        status = WdfIoTargetCreate(device, &a, &c->Targets[i]);
        if (!NT_SUCCESS(status)) break;
        RtlInitUnicodeString(&name, names[i]);
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&p, &name, GENERIC_READ);
        p.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        p.EvtIoTargetQueryRemove = PowerQueryRemove;
        status = WdfIoTargetOpen(c->Targets[i], &p);
        if (!NT_SUCCESS(status)) break;
        if (!WdfIoTargetWdmGetTargetPhysicalDevice(c->Targets[i])) {
            status = STATUS_DEVICE_CONFIGURATION_ERROR; break;
        }
    }
    WdfWaitLockRelease(c->Lock);
    if (NT_SUCCESS(status)) IoInvalidateDeviceRelations(WdfDeviceWdmGetPhysicalDevice(device), PowerRelations);
    return status;
}

_Use_decl_annotations_
NTSTATUS PowerRelease(WDFDEVICE device, WDFCMRESLIST translated)
{
    POWER_CONTEXT *c = PowerContext(device);
    ULONG i;
    UNREFERENCED_PARAMETER(translated);
    WdfWaitLockAcquire(c->Lock, NULL);
    for (i = 0; i < RTL_NUMBER_OF(c->Targets); ++i) {
        if (c->Targets[i]) {
            WdfIoTargetClose(c->Targets[i]); WdfObjectDelete(c->Targets[i]); c->Targets[i] = NULL;
        }
    }
    WdfWaitLockRelease(c->Lock);
    IoInvalidateDeviceRelations(WdfDeviceWdmGetPhysicalDevice(device), PowerRelations);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS PowerAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_PNPPOWER_EVENT_CALLBACKS p;
    UCHAR minor = IRP_MN_QUERY_DEVICE_RELATIONS;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(driver);
    WdfFdoInitSetFilter(init);
    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(init, PowerQueryRelations, IRP_MJ_PNP, &minor, 1);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);
    p.EvtDevicePrepareHardware = PowerPrepare; p.EvtDeviceReleaseHardware = PowerRelease;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &p);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, POWER_CONTEXT);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = device;
    return WdfWaitLockCreate(&a, &PowerContext(device)->Lock);
}

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, PowerAdd);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
