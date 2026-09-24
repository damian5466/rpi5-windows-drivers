/*
 * Copyright (c) Microsoft Corporation. All Rights Reserved.
 * Copyright (c) 2020 Mario Bălănică. All Rights Reserved.
 * Pi 5 vendor initialization, derived from cywbtserialbus. See LICENSE.txt (MS-PL).
 */
#include "driver.h"
#include <acpiioct.h>
#include <gpio.h>
#include <reshub.h>
#include "firmware.h"
#include "device.tmh"

static VOID BtDelay(ULONG Milliseconds)
{
    LARGE_INTEGER delay;
    delay.QuadPart = -10000LL * Milliseconds;
    (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
}
VOID BtCheckpoint(PFDO_EXTENSION c, ULONG Stage, NTSTATUS Status)
{
    WDFKEY key;
    ULONG data[10] = {1, Stage, (ULONG)Status, c->BtFirmwareCommands,
        c->BtBaud, c->BtChipId, c->BtBuild, c->BtBootLineErrors, 0, 0};
    DECLARE_CONST_UNICODE_STRING(name, L"BluetoothDiagnostics");
    RtlCopyMemory(&data[8], c->BtAddress, 6);
    c->BtStage = Stage; c->BtStatus = Status;
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(c->WdfDevice, PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (void)WdfRegistryAssignValue(key, &name, REG_BINARY, sizeof(data), data);
        WdfRegistryClose(key);
    }
    DoTrace(LEVEL_INFO, TFLAG_IO, ("Stage %lu status %!STATUS! firmware commands %lu",
        Stage, Status, c->BtFirmwareCommands));
}
static NTSTATUS BtIoctl(WDFIOTARGET Target, ULONG Code, PVOID Input, ULONG Length)
{
    WDF_MEMORY_DESCRIPTOR input;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input, Input, Length);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(3));
    return WdfIoTargetSendIoctlSynchronously(Target, NULL, Code,
        &input, NULL, &options, NULL);
}
static NTSTATUS BtTransfer(WDFIOTARGET Target, BOOLEAN Write, PUCHAR Buffer, ULONG Length)
{
    ULONG offset = 0;
    ULONGLONG deadline = KeQueryInterruptTime() + 30000000ULL;
    while (offset < Length) {
        ULONG_PTR transferred = 0;
        WDF_MEMORY_DESCRIPTOR memory;
        WDF_REQUEST_SEND_OPTIONS options;
        LONGLONG remaining = (LONGLONG)(deadline - KeQueryInterruptTime());
        NTSTATUS status;
        if (remaining <= 0) return STATUS_IO_TIMEOUT;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, Buffer + offset, Length - offset);
        WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, -remaining);
        status = Write ? WdfIoTargetSendWriteSynchronously(Target, NULL, &memory, NULL, &options, &transferred) :
            WdfIoTargetSendReadSynchronously(Target, NULL, &memory, NULL, &options, &transferred);
        DoTrace(LEVEL_VERBOSE, TFLAG_IO, ("Transfer write %u length %lu offset %lu returned %Iu status %!STATUS!",
            Write, Length, offset, transferred, status));
        if (!NT_SUCCESS(status)) return status;
        if (!transferred || transferred > Length - offset) return STATUS_DEVICE_PROTOCOL_ERROR;
        offset += (ULONG)transferred;
    }
    return STATUS_SUCCESS;
}
// Startup is synchronous and has no read pump. Reject a mismatched event instead
// of silently consuming part of an H4 packet or an unrelated command response.
static NTSTATUS BtCommand(WDFIOTARGET Target, const UCHAR *Command, ULONG Length,
    UCHAR Event[257], ULONG *EventLength)
{
    UCHAR packet[259], type;
    NTSTATUS status;
    if (Length < 3 || Length > 258 || Length != (ULONG)Command[2] + 3)
        return STATUS_INVALID_PARAMETER;
    packet[0] = 1; RtlCopyMemory(packet + 1, Command, Length);
    DoTrace(LEVEL_INFO, TFLAG_IO, ("HCI command %02x%02x length %lu", Command[1], Command[0], Length));
    status = BtTransfer(Target, TRUE, packet, Length + 1);
    if (!NT_SUCCESS(status)) return status;
    status = BtTransfer(Target, FALSE, &type, 1);
    if (!NT_SUCCESS(status)) return status;
    if (type != 4) return STATUS_DEVICE_PROTOCOL_ERROR;
    status = BtTransfer(Target, FALSE, Event, 2);
    if (!NT_SUCCESS(status)) return status;
    status = BtTransfer(Target, FALSE, Event + 2, Event[1]);
    if (!NT_SUCCESS(status)) return status;
    *EventLength = (ULONG)Event[1] + 2;
    DoTrace(LEVEL_INFO, TFLAG_IO, ("HCI event %02x length %u credits %u opcode %02x%02x status %02x",
        Event[0], Event[1], Event[1] >= 1 ? Event[2] : 0,
        Event[1] >= 3 ? Event[4] : 0, Event[1] >= 2 ? Event[3] : 0,
        Event[1] >= 4 ? Event[5] : 0));
    if (Event[0] != 0x0e || Event[1] < 4 || Event[3] != Command[0] ||
        Event[4] != Command[1] || Event[5]) return STATUS_DEVICE_PROTOCOL_ERROR;
    return STATUS_SUCCESS;
}
static NTSTATUS BtIdentity(PFDO_EXTENSION c)
{
    ACPI_EVAL_INPUT_BUFFER input = {0};
    union { ACPI_EVAL_OUTPUT_BUFFER Header; UCHAR Bytes[64]; } result = {0};
    WDF_MEMORY_DESCRIPTOR in, out;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR returned = 0;
    NTSTATUS status;
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(input.MethodName, "BMAC", 4);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &input, sizeof(input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &result, sizeof(result));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(3));
    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(c->WdfDevice), NULL,
        IOCTL_ACPI_EVAL_METHOD, &in, &out, &options, &returned);
    if (!NT_SUCCESS(status)) return status;
    if (returned < FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument[0].Data) + 6 ||
        result.Header.Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || result.Header.Count != 1 ||
        result.Header.Argument[0].Type != ACPI_METHOD_ARGUMENT_BUFFER ||
        result.Header.Argument[0].DataLength != 6) return STATUS_ACPI_INVALID_DATA;
    RtlCopyMemory(c->BtAddress, result.Header.Argument[0].Data, 6);
    return STATUS_SUCCESS;
}
static NTSTATUS BtPin(PFDO_EXTENSION c, UCHAR Value)
{
    WDF_MEMORY_DESCRIPTOR value;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&value, &Value, sizeof(Value));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(3));
    return WdfIoTargetSendIoctlSynchronously(c->IoTargetGPIO, NULL, IOCTL_GPIO_WRITE_PINS,
        &value, &value, &options, NULL);
}
_Use_decl_annotations_
NTSTATUS DeviceEnable(WDFDEVICE Device, BOOLEAN Enabled)
{
    PFDO_EXTENSION c = FdoGetExtension(Device);
    NTSTATUS status;
    SERIAL_HANDFLOW flow = {SERIAL_CTS_HANDSHAKE, 0, 0, 0};
    if (!ValidConnectionID(c->GPIOConnectionId)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!c->IoTargetGPIO) {
        WCHAR path[RESOURCE_HUB_PATH_SIZE / sizeof(WCHAR)];
        UNICODE_STRING name;
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_IO_TARGET_OPEN_PARAMS open;
        RtlInitEmptyUnicodeString(&name, path, sizeof(path));
        status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&name, c->GPIOConnectionId.LowPart, c->GPIOConnectionId.HighPart);
        if (!NT_SUCCESS(status)) return status;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject = Device;
        status = WdfIoTargetCreate(Device, &attributes, &c->IoTargetGPIO);
        if (!NT_SUCCESS(status)) return status;
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&open, &name, GENERIC_WRITE);
        status = WdfIoTargetOpen(c->IoTargetGPIO, &open);
        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(c->IoTargetGPIO); c->IoTargetGPIO = NULL; return status;
        }
    }
    // BT_UART_CTS_N must be high across BT_REG_ON's rising edge. Holding
    // host RTS asserted selects the ROM autobaud/download recovery mode,
    // which answers reset but ignores normal HCI queries. Match hci_bcm's
    // drive_rts_on_open sequence for brcm,bcm43438-bt.
    if (Enabled) {
        status = BtIoctl(c->IoTargetSerial, IOCTL_SERIAL_SET_HANDFLOW, &flow, sizeof(flow));
        if (!NT_SUCCESS(status)) return status;
    }
    status = BtPin(c, 0);
    if (!NT_SUCCESS(status)) return status;
    BtDelay(50);
    if (Enabled) {
        status = BtPin(c, 1);
        if (NT_SUCCESS(status)) {
            BtDelay(150);
            flow.FlowReplace = SERIAL_RTS_HANDSHAKE;
            status = BtIoctl(c->IoTargetSerial, IOCTL_SERIAL_SET_HANDFLOW, &flow, sizeof(flow));
        }
    }
    return status;
}
_Use_decl_annotations_
BOOLEAN DeviceInitialize(PFDO_EXTENSION c, WDFIOTARGET Target, WDFREQUEST Request, BOOLEAN Reset)
{
    static const UCHAR reset[] = {3, 0x0c, 0};
    static const UCHAR name[] = {0x14, 0x0c, 0};
    static const UCHAR verbose[] = {0x79, 0xfc, 0};
    static const UCHAR download[] = {0x2e, 0xfc, 0};
    static const UCHAR address[] = {9, 0x10, 0};
    UCHAR setAddress[] = {1, 0xfc, 6, 0, 0, 0, 0, 0, 0};
    UCHAR baudCommand[] = {0x18, 0xfc, 6, 0, 0, 0, 8, 7, 0}; // 460800
    UCHAR event[257];
    ULONG size = 0, offset = 0, stage = 40;
    SERIAL_BAUD_RATE baud = {115200};
    SERIAL_LINE_CONTROL line = {STOP_BIT_1, NO_PARITY, 8};
    SERIAL_HANDFLOW flow = {SERIAL_CTS_HANDSHAKE, SERIAL_RTS_HANDSHAKE, 0, 0};
    SERIAL_TIMEOUTS timeouts = {0};
    ULONG purge = SERIAL_PURGE_RXCLEAR | SERIAL_PURGE_TXCLEAR;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Request); UNREFERENCED_PARAMETER(Reset);
    c->BtFirmwareCommands = 0; c->BtBaud = 115200; c->BtBootLineErrors = 0;
    BtCheckpoint(c, stage, STATUS_PENDING);
    status = BtIdentity(c); if (!NT_SUCCESS(status)) goto Done;
    stage = 41;
    status = BtIoctl(Target, IOCTL_SERIAL_SET_BAUD_RATE, &baud, sizeof(baud)); if (!NT_SUCCESS(status)) goto Done;
    status = BtIoctl(Target, IOCTL_SERIAL_SET_LINE_CONTROL, &line, sizeof(line)); if (!NT_SUCCESS(status)) goto Done;
    status = BtIoctl(Target, IOCTL_SERIAL_SET_HANDFLOW, &flow, sizeof(flow)); if (!NT_SUCCESS(status)) goto Done;
    status = BtIoctl(Target, IOCTL_SERIAL_SET_TIMEOUTS, &timeouts, sizeof(timeouts)); if (!NT_SUCCESS(status)) goto Done;
    status = BtIoctl(Target, IOCTL_SERIAL_PURGE, &purge, sizeof(purge)); if (!NT_SUCCESS(status)) goto Done;
    stage = 50; BtCheckpoint(c, stage, STATUS_PENDING);
    status = BtCommand(Target, reset, sizeof(reset), event, &size); if (!NT_SUCCESS(status)) goto Done;
    // The BCM ROM acknowledges reset before it can accept the next command.
    // Match btbcm_reset()'s post-completion settling interval.
    BtDelay(100);
    stage = 52;
    status = BtCommand(Target, verbose, sizeof(verbose), event, &size); if (!NT_SUCCESS(status)) goto Done;
    if (size < 12) { status = STATUS_DEVICE_PROTOCOL_ERROR; goto Done; }
    c->BtChipId = event[6]; c->BtBuild = event[10] | ((ULONG)event[11] << 8);
    stage = 51;
    status = BtCommand(Target, name, sizeof(name), event, &size); if (!NT_SUCCESS(status)) goto Done;
    if (size < 16 || RtlCompareMemory(event + 6, "BCM4345C0", 9) != 9 || event[15]) {
        status = STATUS_NOT_SUPPORTED; goto Done;
    }
    // Validate the complete embedded HCD before sending any of its records.
    while (offset < sizeof(BcmFirmware)) {
        ULONG remaining = (ULONG)sizeof(BcmFirmware) - offset;
        if (offset > sizeof(BcmFirmware) - 3 || (ULONG)BcmFirmware[offset + 2] + 3 > remaining) {
            status = STATUS_INVALID_IMAGE_FORMAT; goto Done;
        }
        offset += (ULONG)BcmFirmware[offset + 2] + 3;
    }
    if (!c->BtBuild) {
        stage = 60; BtCheckpoint(c, stage, STATUS_PENDING);
        status = BtCommand(Target, download, sizeof(download), event, &size); if (!NT_SUCCESS(status)) goto Done;
        BtDelay(50);
        for (offset = 0; offset < sizeof(BcmFirmware);) {
            ULONG length;
            if (offset > sizeof(BcmFirmware) - 3) { status = STATUS_INVALID_IMAGE_FORMAT; goto Done; }
            length = (ULONG)BcmFirmware[offset + 2] + 3;
            if (length > sizeof(BcmFirmware) - offset) { status = STATUS_INVALID_IMAGE_FORMAT; goto Done; }
            status = BtCommand(Target, BcmFirmware + offset, length, event, &size);
            if (!NT_SUCCESS(status)) goto Done;
            offset += length; ++c->BtFirmwareCommands;
        }
        BtDelay(250);
        stage = 61;
        {
            SERIAL_STATUS report = {0};
            WDF_MEMORY_DESCRIPTOR output;
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &report, sizeof(report));
            status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SERIAL_GET_COMMSTATUS,
                NULL, &output, NULL, NULL);
            if (!NT_SUCCESS(status)) goto Done;
            c->BtBootLineErrors = report.Errors;
            if (report.Errors & ~(SERIAL_ERROR_BREAK | SERIAL_ERROR_FRAMING)) {
                status = STATUS_DEVICE_PROTOCOL_ERROR; goto Done;
            }
        }
        // Launch RAM briefly drives RX low (a break) as the patched image
        // starts. Discard that boot boundary before beginning a new HCI
        // exchange; normal command/event parsing remains strict.
        purge = SERIAL_PURGE_RXCLEAR;
        status = BtIoctl(Target, IOCTL_SERIAL_PURGE, &purge, sizeof(purge)); if (!NT_SUCCESS(status)) goto Done;
        status = BtCommand(Target, reset, sizeof(reset), event, &size); if (!NT_SUCCESS(status)) goto Done;
        BtDelay(100);
        status = BtCommand(Target, verbose, sizeof(verbose), event, &size); if (!NT_SUCCESS(status)) goto Done;
        if (size < 12 || !(event[10] | event[11])) { status = STATUS_DEVICE_PROTOCOL_ERROR; goto Done; }
        c->BtBuild = event[10] | ((ULONG)event[11] << 8);
    }
    stage = 70; BtCheckpoint(c, stage, STATUS_PENDING);
    RtlCopyMemory(setAddress + 3, c->BtAddress, 6);
    status = BtCommand(Target, setAddress, sizeof(setAddress), event, &size); if (!NT_SUCCESS(status)) goto Done;
    status = BtCommand(Target, address, sizeof(address), event, &size); if (!NT_SUCCESS(status)) goto Done;
    if (size != 12 || RtlCompareMemory(event + 6, c->BtAddress, 6) != 6) {
        status = STATUS_DEVICE_PROTOCOL_ERROR; goto Done;
    }
    stage = 80;
    status = BtCommand(Target, baudCommand, sizeof(baudCommand), event, &size); if (!NT_SUCCESS(status)) goto Done;
    baud.BaudRate = 460800;
    status = BtIoctl(Target, IOCTL_SERIAL_SET_BAUD_RATE, &baud, sizeof(baud)); if (!NT_SUCCESS(status)) goto Done;
    c->BtBaud = baud.BaudRate;
    status = BtCommand(Target, address, sizeof(address), event, &size); if (!NT_SUCCESS(status)) goto Done;
    if (size != 12 || RtlCompareMemory(event + 6, c->BtAddress, 6) != 6) {
        status = STATUS_DEVICE_PROTOCOL_ERROR; goto Done;
    }
    stage = 90;
Done:
    if (!NT_SUCCESS(status)) {
        SERIAL_STATUS serial = {0};
        ULONG modem = 0;
        WDF_MEMORY_DESCRIPTOR output;
        NTSTATUS query;
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &serial, sizeof(serial));
        query = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SERIAL_GET_COMMSTATUS,
            NULL, &output, NULL, NULL);
        if (!NT_SUCCESS(query)) RtlZeroMemory(&serial, sizeof(serial));
        DoTrace(LEVEL_ERROR, TFLAG_IO, ("Serial status %!STATUS! errors %lx in %lu out %lu holds %lx",
            query, serial.Errors, serial.AmountInInQueue, serial.AmountInOutQueue, serial.HoldReasons));
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &modem, sizeof(modem));
        query = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SERIAL_GET_MODEMSTATUS,
            NULL, &output, NULL, NULL);
        if (!NT_SUCCESS(query)) modem = 0;
        DoTrace(LEVEL_ERROR, TFLAG_IO, ("Modem status %!STATUS! lines %lx", query, modem));
    }
    BtCheckpoint(c, stage, status);
    return NT_SUCCESS(status);
}
_Use_decl_annotations_
VOID DeviceQueryDeviceParameters(WDFDRIVER Driver) { UNREFERENCED_PARAMETER(Driver); }
_Use_decl_annotations_
NTSTATUS DeviceEnableWakeControl(WDFDEVICE Device, SYSTEM_POWER_STATE State)
{ UNREFERENCED_PARAMETER(Device); UNREFERENCED_PARAMETER(State); return STATUS_NOT_SUPPORTED; }
VOID DeviceDisableWakeControl(WDFDEVICE Device) { UNREFERENCED_PARAMETER(Device); }
_Use_decl_annotations_
NTSTATUS DevicePowerOn(WDFDEVICE Device) { UNREFERENCED_PARAMETER(Device); return STATUS_NOT_SUPPORTED; }
_Use_decl_annotations_
NTSTATUS DevicePowerOff(WDFDEVICE Device) { return DeviceEnable(Device, FALSE); }
_Use_decl_annotations_
VOID DeviceDoPLDR(WDFDEVICE Device) { UNREFERENCED_PARAMETER(Device); }
