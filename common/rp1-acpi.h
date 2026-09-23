// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <acpiioct.h>
static NTSTATUS Rp1GetUid(WDFDEVICE Device, ULONG *Uid)
{
    ACPI_EVAL_INPUT_BUFFER input;
    ACPI_EVAL_OUTPUT_BUFFER output;
    WDF_MEMORY_DESCRIPTOR in, out;
    NTSTATUS status;
    RtlZeroMemory(&input, sizeof(input)); RtlZeroMemory(&output, sizeof(output));
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(input.MethodName, "_UID", 4);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &input, sizeof(input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &output, sizeof(output));
    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device), NULL,
        IOCTL_ACPI_EVAL_METHOD, &in, &out, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (output.Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || output.Count != 1 ||
        output.Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER || output.Argument[0].DataLength != sizeof(ULONG))
        return STATUS_ACPI_INVALID_DATA;
    *Uid = output.Argument[0].Argument;
    return STATUS_SUCCESS;
}
