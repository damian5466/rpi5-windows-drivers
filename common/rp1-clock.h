// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#define RP1_CLOCK_NAME L"\\Device\\Pi5Rp1Clock"
#define IOCTL_RP1_CLOCK_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_RP1_CLOCK_UART CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_WRITE_ACCESS)
typedef struct {
    ULONG Version, Online, SystemHz, UartHz, UartUsers;
    ULONG SystemControl, SystemDivider, UartControl, UartDivider;
} RP1_CLOCK_STATUS;
#ifdef RP1_CLOCK_CLIENT
static NTSTATUS Rp1OpenClock(WDFDEVICE Device, BOOLEAN Uart, WDFIOTARGET *Target, ULONG *Hz)
{
    WDF_OBJECT_ATTRIBUTES a;
    WDF_IO_TARGET_OPEN_PARAMS open;
    WDF_MEMORY_DESCRIPTOR output;
    RP1_CLOCK_STATUS state = {0};
    ULONG_PTR bytes = 0;
    UNICODE_STRING name = RTL_CONSTANT_STRING(RP1_CLOCK_NAME);
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = Device;
    status = WdfIoTargetCreate(Device, &a, Target);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_READ | GENERIC_WRITE);
    open.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    status = WdfIoTargetOpen(*Target, &open);
    if (NT_SUCCESS(status)) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &state, sizeof(state));
        status = WdfIoTargetSendIoctlSynchronously(*Target, NULL,
            Uart ? IOCTL_RP1_CLOCK_UART : IOCTL_RP1_CLOCK_QUERY, NULL, &output, NULL, &bytes);
        if (NT_SUCCESS(status)) {
            *Hz = Uart ? state.UartHz : state.SystemHz;
            if (bytes != sizeof(state) || state.Version != 1 || !state.Online || !*Hz)
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    if (!NT_SUCCESS(status)) { WdfObjectDelete(*Target); *Target = NULL; }
    return status;
}
#endif
