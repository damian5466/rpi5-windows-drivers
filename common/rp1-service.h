// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// One RP1 service owns the interrupt router. Each kernel client keeps an open
// target for its lease; closing it disables only the routes that client owns.
#define RP1_SERVICE_NAME L"\\Device\\Pi5Rp1"
#define IOCTL_RP1_ACQUIRE_IRQ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_RP1_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define RP1_SERVICE_VERSION 1
typedef struct { ULONG Version; ULONG Source; } RP1_IRQ_REQUEST;
typedef struct {
    ULONG Version;
    ULONG Online;
    ULONGLONG Owned;
    ULONG Config[61];
} RP1_SERVICE_STATUS;

#ifdef RP1_SERVICE_CLIENT
static NTSTATUS Rp1OpenInterruptRoute(WDFDEVICE Device, ULONG Source, WDFIOTARGET *Target)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_TARGET_OPEN_PARAMS open;
    WDF_MEMORY_DESCRIPTOR input;
    RP1_IRQ_REQUEST request = { RP1_SERVICE_VERSION, Source };
    UNICODE_STRING name = RTL_CONSTANT_STRING(RP1_SERVICE_NAME);
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfIoTargetCreate(Device, &attributes, Target);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_READ | GENERIC_WRITE);
    open.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    status = WdfIoTargetOpen(*Target, &open);
    if (NT_SUCCESS(status)) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input, &request, sizeof(request));
        status = WdfIoTargetSendIoctlSynchronously(*Target, NULL,
            IOCTL_RP1_ACQUIRE_IRQ, &input, NULL, NULL, NULL);
    }
    if (!NT_SUCCESS(status)) { WdfObjectDelete(*Target); *Target = NULL; }
    return status;
}
#endif
