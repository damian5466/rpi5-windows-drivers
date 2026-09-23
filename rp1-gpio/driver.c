// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"
#define RP1_SERVICE_CLIENT
#include "rp1-service.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD GpioAdd;
EVT_WDF_DRIVER_UNLOAD GpioUnload;

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    GPIO_CLIENT_REGISTRATION_PACKET p;
    WDFDRIVER driver;
    NTSTATUS status;
    WDF_DRIVER_CONFIG_INIT(&config, GpioAdd);
    config.EvtDriverUnload = GpioUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) return status;
    RtlZeroMemory(&p, sizeof(p));
    p.Version = GPIO_CLIENT_VERSION; p.Size = sizeof(p);
    p.ControllerContextSize = sizeof(GPIO_CONTEXT);
    p.CLIENT_PrepareController = GpioPrepare;
    p.CLIENT_ReleaseController = GpioRelease;
    p.CLIENT_StartController = GpioStart;
    p.CLIENT_StopController = GpioStop;
    p.CLIENT_QueryControllerBasicInformation = GpioInformation;
    p.CLIENT_QuerySetControllerInformation = GpioQuerySet;
    p.CLIENT_ConnectIoPins = GpioConnect;
    p.CLIENT_DisconnectIoPins = GpioDisconnect;
    p.CLIENT_ReadGpioPinsUsingMask = GpioReadPins;
    p.CLIENT_WriteGpioPinsUsingMask = GpioWritePins;
    p.CLIENT_EnableInterrupt = GpioEnableInterrupt;
    p.CLIENT_DisableInterrupt = GpioDisableInterrupt;
    p.CLIENT_MaskInterrupts = GpioMaskInterrupts;
    p.CLIENT_UnmaskInterrupt = GpioUnmaskInterrupt;
    p.CLIENT_QueryActiveInterrupts = GpioQueryInterrupts;
    p.CLIENT_QueryEnabledInterrupts = GpioQueryEnabled;
    p.CLIENT_ClearActiveInterrupts = GpioClearInterrupts;
    p.CLIENT_ReconfigureInterrupt = GpioReconfigureInterrupt;
    p.CLIENT_ConnectFunctionConfigPins = GpioConnectFunction;
    p.CLIENT_DisconnectFunctionConfigPins = GpioDisconnectFunction;
    return GPIO_CLX_RegisterClient(driver, &p, RegistryPath);
}
_Use_decl_annotations_
VOID GpioUnload(WDFDRIVER Driver) { (void)GPIO_CLX_UnregisterClient(Driver); }
_Use_decl_annotations_
NTSTATUS GpioAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    NTSTATUS status = GPIO_CLX_ProcessAddDevicePreDeviceCreate(Driver, Init, &attributes);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreate(&Init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    return GPIO_CLX_ProcessAddDevicePostDeviceCreate(Driver, device);
}
_Use_decl_annotations_
NTSTATUS GpioPrepare(WDFDEVICE Device, PVOID Context, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    GPIO_CONTEXT *c = Context;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR registers[3] = {0};
    ULONG i, count = 0, interrupts = 0;
    UNREFERENCED_PARAMETER(Raw);
    c->Device = Device;
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (r->Type == CmResourceTypeMemory) {
            if (count == 3 || r->u.Memory.Length != 0xc000) return STATUS_DEVICE_CONFIGURATION_ERROR;
            registers[count++] = r;
        } else if (r->Type == CmResourceTypeInterrupt) {
            if ((r->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED)) ||
                r->ShareDisposition != CmResourceShareShared) return STATUS_DEVICE_CONFIGURATION_ERROR;
            ++interrupts;
        }
    }
    if (count != 3 || interrupts != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->Io = MmMapIoSpaceEx(registers[0]->u.Memory.Start, 0xc000, PAGE_READWRITE | PAGE_NOCACHE);
    c->Rio = MmMapIoSpaceEx(registers[1]->u.Memory.Start, 0xc000, PAGE_READWRITE | PAGE_NOCACHE);
    c->Pads = MmMapIoSpaceEx(registers[2]->u.Memory.Start, 0xc000, PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->Io || !c->Rio || !c->Pads) {
        (void)GpioRelease(Device, Context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (GpioRead(c->Io, RP1_INTE)) {
        (void)GpioRelease(Device, Context);
        return STATUS_DEVICE_BUSY;
    }
    for (i = 2; i < RP1_TOTAL_PINS; ++i)
        if (RP1_ALLOWED_MASK & (1ull << i)) GpioSnapshot(c, i, &c->Boot[i]);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioRelease(WDFDEVICE Device, PVOID Context)
{
    GPIO_CONTEXT *c = Context;
    UNREFERENCED_PARAMETER(Device);
    if (c->Route) { WdfObjectDelete(c->Route); c->Route = NULL; }
    if (c->Io) { MmUnmapIoSpace(c->Io, 0xc000); c->Io = NULL; }
    if (c->Rio) { MmUnmapIoSpace(c->Rio, 0xc000); c->Rio = NULL; }
    if (c->Pads) { MmUnmapIoSpace(c->Pads, 0xc000); c->Pads = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioInformation(PVOID Context, PCLIENT_CONTROLLER_BASIC_INFORMATION Information)
{
    UNREFERENCED_PARAMETER(Context);
    RtlZeroMemory(Information, sizeof(*Information));
    Information->Version = GPIO_CONTROLLER_BASIC_INFORMATION_VERSION;
    Information->Size = sizeof(*Information);
    Information->TotalPins = RP1_TOTAL_PINS;
    Information->NumberOfPinsPerBank = RP1_TOTAL_PINS;
    Information->Flags.MemoryMappedController = 1;
    Information->Flags.FormatIoRequestsAsMasks = 1;
    Information->Flags.EmulateDebouncing = 1;
    Information->Flags.EmulateActiveBoth = 1; // Required by Resource Hub Proxy.
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioQuerySet(PVOID Context, PCLIENT_CONTROLLER_QUERY_SET_INFORMATION_INPUT Input,
    PCLIENT_CONTROLLER_QUERY_SET_INFORMATION_OUTPUT Output)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Input);
    UNREFERENCED_PARAMETER(Output);
    return STATUS_NOT_SUPPORTED;
}
_Use_decl_annotations_
NTSTATUS GpioStart(PVOID Context, BOOLEAN RestoreContext, WDF_POWER_DEVICE_STATE Previous)
{
    GPIO_CONTEXT *c = Context;
    NTSTATUS status;
    ULONG pin;
    UNREFERENCED_PARAMETER(Previous);
    status = Rp1OpenInterruptRoute(c->Device, 0, &c->Route);
    if (!NT_SUCCESS(status)) return status;
    if (RestoreContext) {
        for (pin = 2; pin < RP1_TOTAL_PINS; ++pin)
            if (c->Touched & (1ull << pin)) GpioRestore(c, pin, &c->Resume[pin]);
        GpioWrite(c->Io, RP1_SET + RP1_INTE, c->ResumeInte);
    }
    c->Started = TRUE;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "Pi5Gpio: header bank online, shared INTA route 0\n");
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioStop(PVOID Context, BOOLEAN SaveContext, WDF_POWER_DEVICE_STATE Target)
{
    GPIO_CONTEXT *c = Context;
    ULONG pin;
    UNREFERENCED_PARAMETER(Target);
    if (SaveContext) {
        c->ResumeInte = GpioRead(c->Io, RP1_INTE) & c->IrqOwned;
        for (pin = 2; pin < RP1_TOTAL_PINS; ++pin)
            if (c->Touched & (1ull << pin)) GpioSnapshot(c, pin, &c->Resume[pin]);
    }
    GpioWrite(c->Io, RP1_CLEAR + RP1_INTE, c->IrqOwned);
    for (pin = 2; pin < RP1_TOTAL_PINS; ++pin)
        if (c->Touched & (1ull << pin)) GpioRestore(c, pin, &c->Boot[pin]);
    c->Started = FALSE;
    if (c->Route) { WdfObjectDelete(c->Route); c->Route = NULL; }
    return STATUS_SUCCESS;
}

static VOID RestoreUnowned(GPIO_CONTEXT *c, ULONGLONG mask)
{
    ULONG pin;
    mask &= ~(c->IoOwned | c->IrqOwned | c->FunctionOwned);
    for (pin = 2; pin < RP1_TOTAL_PINS; ++pin) if (mask & (1ull << pin)) {
        GpioRestore(c, pin, &c->Boot[pin]);
        c->Touched &= ~(1ull << pin);
    }
}
_Use_decl_annotations_
NTSTATUS GpioConnect(PVOID Context, PGPIO_CONNECT_IO_PINS_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    ULONGLONG mask;
    ULONG pin;
    NTSTATUS status = GpioPinMask(p->BankId, p->PinNumberTable, p->PinCount, &mask);
    if (!NT_SUCCESS(status)) return status;
    if ((mask & RP1_BOARD_MASK) && p->ConnectMode != ConnectModeOutput) return STATUS_NOT_SUPPORTED;
    if ((p->ConnectMode != ConnectModeInput && p->ConnectMode != ConnectModeOutput) ||
        p->PullConfiguration > GPIO_PIN_PULL_CONFIGURATION_NONE || p->VendorDataLength ||
        (p->DriveStrength && p->DriveStrength != 200 && p->DriveStrength != 400 &&
         p->DriveStrength != 800 && p->DriveStrength != 1200)) return STATUS_NOT_SUPPORTED;
    status = GpioCanClaim(c, mask);
    if (!NT_SUCCESS(status)) return status;
    GPIO_CLX_AcquireInterruptLock(Context, 0);
    if ((mask & (c->IoOwned | c->FunctionOwned)) ||
        (p->ConnectMode == ConnectModeOutput && (mask & c->IrqOwned))) status = STATUS_SHARING_VIOLATION;
    else {
        for (pin = 2; pin < RP1_TOTAL_PINS; ++pin) if (mask & (1ull << pin))
            GpioConfigure(c, pin, 5, p->ConnectMode == ConnectModeOutput, p->PullConfiguration, p->DriveStrength);
        c->IoOwned |= mask;
        if (p->ConnectMode == ConnectModeOutput) c->OutputOwned |= mask;
    }
    GPIO_CLX_ReleaseInterruptLock(Context, 0);
    return status;
}
_Use_decl_annotations_
NTSTATUS GpioDisconnect(PVOID Context, PGPIO_DISCONNECT_IO_PINS_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    ULONGLONG mask;
    NTSTATUS status = GpioPinMask(p->BankId, p->PinNumberTable, p->PinCount, &mask);
    if (!NT_SUCCESS(status)) return status;
    GPIO_CLX_AcquireInterruptLock(Context, 0);
    c->IoOwned &= ~mask; c->OutputOwned &= ~mask;
    if (!p->DisconnectFlags.PreserveConfiguration) RestoreUnowned(c, mask);
    GPIO_CLX_ReleaseInterruptLock(Context, 0);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioReadPins(PVOID Context, PGPIO_READ_PINS_MASK_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    if (p->BankId || !c->Started) return STATUS_INVALID_PARAMETER;
    // GpioClx also reads interrupt-only pins for active-both emulation.
    // It applies the caller's connection mask after this bank-wide read.
    *p->PinValues = GpioReadValues(c, p->Flags.WriteConfiguredPins != 0);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioWritePins(PVOID Context, PGPIO_WRITE_PINS_MASK_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    // The class extension can prime the output latch before connecting the
    // output. Pin access is arbitrated by GpioClx; do not require OutputOwned.
    if (p->BankId || !c->Started || ((p->SetMask | p->ClearMask) & ~RP1_ALLOWED_MASK) ||
        (p->SetMask & p->ClearMask)) return STATUS_INVALID_PARAMETER;
    GpioWriteValues(c, p->SetMask, p->ClearMask);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS GpioEnableInterrupt(PVOID Context, PGPIO_ENABLE_INTERRUPT_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    ULONG bit, events;
    NTSTATUS status = GpioIrqPinMask(p->BankId, p->PinNumber, &bit);
    if (!NT_SUCCESS(status)) return status;
    if (p->PullConfiguration > GPIO_PIN_PULL_CONFIGURATION_NONE || p->VendorDataLength)
        return STATUS_NOT_SUPPORTED;
    status = GpioTrigger(p->InterruptMode, p->Polarity, &events);
    if (!NT_SUCCESS(status)) return status;
    status = GpioCanClaim(c, bit);
    if (!NT_SUCCESS(status)) return status;
    GPIO_CLX_AcquireInterruptLock(Context, 0);
    if (bit & (c->FunctionOwned | c->OutputOwned | c->IrqOwned)) status = STATUS_SHARING_VIOLATION;
    else {
        GpioWrite(c->Io, RP1_CLEAR + RP1_INTE, bit);
        GpioConfigure(c, p->PinNumber, 5, FALSE, p->PullConfiguration, 0);
        GpioWrite(c->Io, RP1_SET + RP1_CTRL(p->PinNumber), RP1_IRQ_RESET);
        GpioWrite(c->Io, RP1_SET + RP1_CTRL(p->PinNumber), events);
        c->IrqOwned |= bit;
        GpioWrite(c->Io, RP1_SET + RP1_INTE, bit);
        (void)GpioRead(c->Io, RP1_INTE);
    }
    GPIO_CLX_ReleaseInterruptLock(Context, 0);
    return status;
}
_Use_decl_annotations_
NTSTATUS GpioDisableInterrupt(PVOID Context, PGPIO_DISABLE_INTERRUPT_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    ULONG bit;
    NTSTATUS status = GpioIrqPinMask(p->BankId, p->PinNumber, &bit);
    if (!NT_SUCCESS(status)) return status;
    GPIO_CLX_AcquireInterruptLock(Context, 0);
    GpioWrite(c->Io, RP1_CLEAR + RP1_INTE, bit);
    GpioWrite(c->Io, RP1_CLEAR + RP1_CTRL(p->PinNumber), RP1_IRQ_EVENTS);
    GpioWrite(c->Io, RP1_SET + RP1_CTRL(p->PinNumber), RP1_IRQ_RESET);
    c->IrqOwned &= ~bit;
    RestoreUnowned(c, bit);
    (void)GpioRead(c->Io, RP1_INTE);
    GPIO_CLX_ReleaseInterruptLock(Context, 0);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioMaskInterrupts(PVOID Context, PGPIO_MASK_INTERRUPT_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    p->FailedMask = p->PinMask;
    if (p->BankId) return STATUS_INVALID_PARAMETER;
    // Mask/clear can precede EnableInterrupt or follow DisableInterrupt.
    GpioWrite(c->Io, RP1_CLEAR + RP1_INTE, (ULONG)p->PinMask & RP1_HEADER_MASK);
    (void)GpioRead(c->Io, RP1_INTE);
    p->FailedMask = 0;
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioUnmaskInterrupt(PVOID Context, PGPIO_ENABLE_INTERRUPT_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    ULONG bit;
    NTSTATUS status = GpioIrqPinMask(p->BankId, p->PinNumber, &bit);
    if (!NT_SUCCESS(status)) return status;
    GpioWrite(c->Io, RP1_SET + RP1_INTE, bit);
    (void)GpioRead(c->Io, RP1_INTE);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioQueryInterrupts(PVOID Context, PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    p->ActiveMask = 0;
    if (p->BankId) return STATUS_INVALID_PARAMETER;
    p->ActiveMask = GpioRead(c->Io, RP1_INTS) & RP1_HEADER_MASK & p->EnabledMask;
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioQueryEnabled(PVOID Context, PGPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    p->EnabledMask = 0;
    if (p->BankId) return STATUS_INVALID_PARAMETER;
    p->EnabledMask = GpioRead(c->Io, RP1_INTE) & RP1_HEADER_MASK;
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioClearInterrupts(PVOID Context, PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    ULONG pin;
    p->FailedClearMask = p->ClearActiveMask;
    if (p->BankId) return STATUS_INVALID_PARAMETER;
    for (pin = 2; pin < RP1_HEADER_PINS; ++pin) if (p->ClearActiveMask & (1ull << pin))
        GpioWrite(c->Io, RP1_SET + RP1_CTRL(pin), RP1_IRQ_RESET);
    (void)GpioRead(c->Io, RP1_INTS);
    p->FailedClearMask = 0;
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioReconfigureInterrupt(PVOID Context, PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    ULONG bit, events, enabled;
    NTSTATUS status = GpioIrqPinMask(p->BankId, p->PinNumber, &bit);
    if (!NT_SUCCESS(status)) return status;
    status = GpioTrigger(p->InterruptMode, p->Polarity, &events);
    if (!NT_SUCCESS(status)) return status;
    enabled = GpioRead(c->Io, RP1_INTE) & bit;
    GpioWrite(c->Io, RP1_CLEAR + RP1_INTE, bit);
    GpioWrite(c->Io, RP1_CLEAR + RP1_CTRL(p->PinNumber), RP1_IRQ_EVENTS);
    GpioWrite(c->Io, RP1_SET + RP1_CTRL(p->PinNumber), RP1_IRQ_RESET);
    GpioWrite(c->Io, RP1_SET + RP1_CTRL(p->PinNumber), events);
    GpioWrite(c->Io, RP1_SET + RP1_INTE, enabled);
    (void)GpioRead(c->Io, RP1_INTE);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS GpioConnectFunction(PVOID Context, PGPIO_CONNECT_FUNCTION_CONFIG_PINS_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    ULONGLONG mask;
    ULONG pin;
    NTSTATUS status = GpioPinMask(p->BankId, p->PinNumberTable, p->PinCount, &mask);
    if (!NT_SUCCESS(status)) return status;
    if ((mask & RP1_BOARD_MASK) || p->FunctionNumber > 8 || p->FunctionNumber == 5 ||
        p->PullConfiguration > GPIO_PIN_PULL_CONFIGURATION_NONE || p->VendorDataLength)
        return STATUS_NOT_SUPPORTED;
    status = GpioCanClaim(c, mask);
    if (!NT_SUCCESS(status)) return status;
    GPIO_CLX_AcquireInterruptLock(Context, 0);
    if (mask & (c->IoOwned | c->IrqOwned | c->FunctionOwned)) status = STATUS_SHARING_VIOLATION;
    else {
        for (pin = 2; pin < RP1_HEADER_PINS; ++pin) if (mask & (1ull << pin))
            GpioConfigure(c, pin, p->FunctionNumber, FALSE, p->PullConfiguration, 0);
        c->FunctionOwned |= mask;
    }
    GPIO_CLX_ReleaseInterruptLock(Context, 0);
    return status;
}
_Use_decl_annotations_
NTSTATUS GpioDisconnectFunction(PVOID Context, PGPIO_DISCONNECT_FUNCTION_CONFIG_PINS_PARAMETERS p)
{
    GPIO_CONTEXT *c = Context;
    ULONGLONG mask;
    NTSTATUS status = GpioPinMask(p->BankId, p->PinNumberTable, p->PinCount, &mask);
    if (!NT_SUCCESS(status)) return status;
    GPIO_CLX_AcquireInterruptLock(Context, 0);
    c->FunctionOwned &= ~mask;
    RestoreUnowned(c, mask);
    GPIO_CLX_ReleaseInterruptLock(Context, 0);
    return STATUS_SUCCESS;
}
