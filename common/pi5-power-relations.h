/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once

/* Caller serializes target open/close, and calls only for PowerRelations at
 * PASSIVE_LEVEL. These are providers that must remain in D0 until this
 * consumer finishes its system-power transition. Opening an I/O target alone
 * does not establish that ordering between separate ACPI devices. */
static NTSTATUS Pi5AppendPowerRelations(PIRP irp, const WDFIOTARGET *targets, ULONG count)
{
    PDEVICE_RELATIONS old, relations;
    PDEVICE_OBJECT parents[8];
    ULONG i, j, oldCount, added = 0;
    SIZE_T bytes;
    PAGED_CODE();
    if (count > RTL_NUMBER_OF(parents)) return STATUS_INVALID_PARAMETER;
    if (!NT_SUCCESS(irp->IoStatus.Status) && irp->IoStatus.Status != STATUS_NOT_SUPPORTED)
        return irp->IoStatus.Status;
    old = (PDEVICE_RELATIONS)irp->IoStatus.Information;
    oldCount = old ? old->Count : 0;
    for (i = 0; i < count; ++i) {
        PDEVICE_OBJECT pdo;
        if (!targets[i]) continue;
        pdo = WdfIoTargetWdmGetTargetPhysicalDevice(targets[i]);
        if (!pdo) return STATUS_DEVICE_CONFIGURATION_ERROR;
        for (j = 0; j < oldCount; ++j) if (old->Objects[j] == pdo) break;
        if (j != oldCount) continue;
        for (j = 0; j < added; ++j) if (parents[j] == pdo) break;
        if (j == added) parents[added++] = pdo;
    }
    if (!added) return STATUS_SUCCESS;
    if (oldCount > MAXULONG - added) return STATUS_INTEGER_OVERFLOW;
    bytes = FIELD_OFFSET(DEVICE_RELATIONS, Objects) + ((SIZE_T)oldCount + added) * sizeof(PDEVICE_OBJECT);
    relations = ExAllocatePool2(POOL_FLAG_PAGED, bytes, 'rP5P');
    if (!relations) return STATUS_INSUFFICIENT_RESOURCES;
    relations->Count = oldCount + added;
    if (oldCount) RtlCopyMemory(relations->Objects, old->Objects, oldCount * sizeof(PDEVICE_OBJECT));
    for (i = 0; i < added; ++i) {
        ObReferenceObject(parents[i]);
        relations->Objects[oldCount + i] = parents[i];
    }
    if (old) ExFreePool(old); /* Existing PDO references transfer to the new array. */
    irp->IoStatus.Information = (ULONG_PTR)relations;
    irp->IoStatus.Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}
