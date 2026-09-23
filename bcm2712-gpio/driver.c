// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD BcmAdd;
EVT_WDF_DRIVER_UNLOAD BcmUnload;
EVT_WDF_TIMER BcmTelemetry;

static VOID Lock(BCM_GPIO *c)
{ if (!c->Uid) GPIO_CLX_AcquireInterruptLock(c, 0); }
static VOID Unlock(BCM_GPIO *c)
{ if (!c->Uid) GPIO_CLX_ReleaseInterruptLock(c, 0); }

// GpioClx serializes the AON controller at PASSIVE_LEVEL, allowing _DSM I/O.
// The main controller is one logical bank, so its shared mux words and both
// hardware data banks are covered by the same GpioClx interrupt lock.
_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    GPIO_CLIENT_REGISTRATION_PACKET p;
    WDFDRIVER driver;
    NTSTATUS status;
    WDF_DRIVER_CONFIG_INIT(&config, BcmAdd); config.EvtDriverUnload = BcmUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
    if (!NT_SUCCESS(status)) return status;
    RtlZeroMemory(&p, sizeof(p));
    p.Version = GPIO_CLIENT_VERSION; p.Size = sizeof(p); p.ControllerContextSize = sizeof(BCM_GPIO);
    p.CLIENT_PrepareController = BcmPrepare; p.CLIENT_ReleaseController = BcmRelease;
    p.CLIENT_StartController = BcmStart; p.CLIENT_StopController = BcmStop;
    p.CLIENT_QueryControllerBasicInformation = BcmInformation;
    p.CLIENT_QuerySetControllerInformation = BcmQuerySet;
    p.CLIENT_ConnectIoPins = BcmConnect; p.CLIENT_DisconnectIoPins = BcmDisconnect;
    p.CLIENT_ReadGpioPinsUsingMask = BcmReadPins; p.CLIENT_WriteGpioPinsUsingMask = BcmWritePins;
    p.CLIENT_EnableInterrupt = BcmEnableInterrupt; p.CLIENT_DisableInterrupt = BcmDisableInterrupt;
    p.CLIENT_MaskInterrupts = BcmMaskInterrupts; p.CLIENT_UnmaskInterrupt = BcmUnmaskInterrupt;
    p.CLIENT_QueryActiveInterrupts = BcmQueryInterrupts; p.CLIENT_QueryEnabledInterrupts = BcmQueryEnabled;
    p.CLIENT_ClearActiveInterrupts = BcmClearInterrupts; p.CLIENT_ReconfigureInterrupt = BcmReconfigureInterrupt;
    p.CLIENT_ConnectFunctionConfigPins = BcmConnectFunction;
    p.CLIENT_DisconnectFunctionConfigPins = BcmDisconnectFunction;
    return GPIO_CLX_RegisterClient(driver, &p, RegistryPath);
}
_Use_decl_annotations_
VOID BcmUnload(WDFDRIVER Driver) { (void)GPIO_CLX_UnregisterClient(Driver); }
_Use_decl_annotations_
NTSTATUS BcmAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    WDF_OBJECT_ATTRIBUTES a;
    WDFDEVICE device;
    NTSTATUS status = GPIO_CLX_ProcessAddDevicePreDeviceCreate(Driver, Init, &a);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreate(&Init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    return GPIO_CLX_ProcessAddDevicePostDeviceCreate(Driver, device);
}

_Use_decl_annotations_
NTSTATUS BcmPrepare(WDFDEVICE Device, PVOID Context, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    BCM_GPIO *c = Context;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR memory[3] = {0};
    ULONG count = 0, interrupts = 0, i;
    ULONGLONG data;
    WDF_TIMER_CONFIG timer;
    WDF_OBJECT_ATTRIBUTES a;
    NTSTATUS status, cleanupStatus;
    UNREFERENCED_PARAMETER(Raw);
    c->Device = Device;
    status = BcmEvalInteger(Device, "_UID", &c->Uid);
    if (!NT_SUCCESS(status)) return status;
    status = BcmEvalInteger(Device, "_HRV", &c->Revision);
    if (!NT_SUCCESS(status)) return status;
    if (c->Uid > 1 || c->Revision > 1) return STATUS_NOT_SUPPORTED;
    c->Valid = BcmValidPins(c->Revision, c->Uid);
    c->PinctrlLength = c->Uid ? (c->Revision ? 0x1c : 0x20) : (c->Revision ? 0x20 : 0x30);
    for (i = 0; i < WdfCmResourceListGetCount(Translated); ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(Translated, i);
        if (!r) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (r->Type == CmResourceTypeMemory) {
            if (count == 3) return STATUS_DEVICE_CONFIGURATION_ERROR;
            memory[count++] = r;
        } else if (r->Type == CmResourceTypeInterrupt) {
            if (r->Flags & (CM_RESOURCE_INTERRUPT_MESSAGE | CM_RESOURCE_INTERRUPT_LATCHED))
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            ++interrupts;
        }
    }
    if (!memory[0] || !memory[1] || (!c->Uid && !memory[2]) ||
        count != (c->Uid ? 2u : 3u) || interrupts != (c->Uid ? 0u : 1u) ||
        memory[0]->u.Memory.Length != 0x40 || memory[1]->u.Memory.Length != c->PinctrlLength ||
        (!c->Uid && memory[2]->u.Memory.Length != 0x10)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->Gio = MmMapIoSpaceEx(memory[0]->u.Memory.Start, 0x40, PAGE_READWRITE | PAGE_NOCACHE);
    c->Pinctrl = MmMapIoSpaceEx(memory[1]->u.Memory.Start, c->PinctrlLength, PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->Uid) c->L2 = MmMapIoSpaceEx(memory[2]->u.Memory.Start, 0x10, PAGE_READWRITE | PAGE_NOCACHE);
    if (!c->Gio || !c->Pinctrl || (!c->Uid && !c->L2)) { status = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
    if (!c->Uid && BcmReadBanks(c, BCM_MASK)) { status = STATUS_DEVICE_BUSY; goto fail; }
    status = BcmReadData(c, &data);
    if (!NT_SUCCESS(status)) goto fail;
    for (i = 0; i < 64; ++i) if (c->Valid & (1ull << i)) BcmSnapshot(c, i, data, &c->Boot[i]);
    status = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &c->Key);
    if (!NT_SUCCESS(status)) goto fail;
    WDF_TIMER_CONFIG_INIT(&timer, BcmTelemetry); timer.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, BCM_TIMER);
    a.ParentObject = Device; a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timer, &a, &c->Timer);
    if (!NT_SUCCESS(status)) goto fail;
    BcmTimerContext(c->Timer)->Controller = c;
    return STATUS_SUCCESS;
fail:
    cleanupStatus = BcmRelease(Device, Context);
    if (!NT_SUCCESS(cleanupStatus)) return cleanupStatus;
    return status;
}
_Use_decl_annotations_
NTSTATUS BcmRelease(WDFDEVICE Device, PVOID Context)
{
    BCM_GPIO *c = Context;
    UNREFERENCED_PARAMETER(Device);
    c->Started = FALSE;
    if (c->Timer) { WdfTimerStop(c->Timer, TRUE); WdfObjectDelete(c->Timer); c->Timer = NULL; }
    if (c->Key) { WdfRegistryClose(c->Key); c->Key = NULL; }
    if (c->L2) { MmUnmapIoSpace(c->L2, 0x10); c->L2 = NULL; }
    if (c->Pinctrl) { MmUnmapIoSpace(c->Pinctrl, c->PinctrlLength); c->Pinctrl = NULL; }
    if (c->Gio) { MmUnmapIoSpace(c->Gio, 0x40); c->Gio = NULL; }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS BcmInformation(PVOID Context, PCLIENT_CONTROLLER_BASIC_INFORMATION p)
{
    BCM_GPIO *c = Context;
    RtlZeroMemory(p, sizeof(*p));
    p->Version = GPIO_CONTROLLER_BASIC_INFORMATION_VERSION; p->Size = sizeof(*p);
    p->TotalPins = c->Uid ? 38 : (c->Revision ? 36 : 54);
    // GpioClx rejects a bank width greater than the total pin count, even
    // when the register mask representation is 64 bits wide.
    p->NumberOfPinsPerBank = (UCHAR)p->TotalPins;
    p->Flags.MemoryMappedController = !c->Uid;
    p->Flags.FormatIoRequestsAsMasks = 1; p->Flags.EmulateDebouncing = 1;
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS BcmQuerySet(PVOID Context, PCLIENT_CONTROLLER_QUERY_SET_INFORMATION_INPUT Input,
    PCLIENT_CONTROLLER_QUERY_SET_INFORMATION_OUTPUT Output)
{
    UNREFERENCED_PARAMETER(Context); UNREFERENCED_PARAMETER(Input); UNREFERENCED_PARAMETER(Output);
    return STATUS_NOT_SUPPORTED;
}
_Use_decl_annotations_
NTSTATUS BcmStart(PVOID Context, BOOLEAN RestoreContext, WDF_POWER_DEVICE_STATE Previous)
{
    BCM_GPIO *c = Context;
    ULONG pin;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(Previous);
    if (RestoreContext) {
        for (pin = 0; pin < 64; ++pin) if (c->Touched & (1ull << pin)) {
            status = BcmRestore(c, pin, &c->Resume[pin]);
            if (!NT_SUCCESS(status)) return status;
        }
        if (!c->Uid) BcmUpdate(c, BCM_MASK, c->IrqOwned, c->ResumeMask);
    }
    if (!c->Uid) {
        c->L2WasMasked = (BcmRead(c->L2, 4) & 1) != 0;
        BcmWrite(c->L2, 12, 1); // Unmask only the GPIO aggregate, preserving other sources.
        (void)BcmRead(c->L2, 4);
    }
    c->Started = TRUE;
    WdfTimerStart(c->Timer, WDF_REL_TIMEOUT_IN_MS(1000));
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
        "Pi5BcmGpio: controller %lu revision %lu online\n", c->Uid, c->Revision);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS BcmStop(PVOID Context, BOOLEAN SaveContext, WDF_POWER_DEVICE_STATE Target)
{
    BCM_GPIO *c = Context;
    ULONG pin;
    ULONGLONG data;
    NTSTATUS result = STATUS_SUCCESS, status;
    UNREFERENCED_PARAMETER(Target);
    c->Started = FALSE;
    WdfTimerStop(c->Timer, TRUE);
    if (SaveContext) {
        status = BcmReadData(c, &data);
        if (!NT_SUCCESS(status)) return status;
        c->ResumeMask = c->Uid ? 0 : BcmReadBanks(c, BCM_MASK) & c->IrqOwned;
        for (pin = 0; pin < 64; ++pin) if (c->Touched & (1ull << pin)) BcmSnapshot(c, pin, data, &c->Resume[pin]);
    }
    if (!c->Uid) {
        BcmUpdate(c, BCM_MASK, c->IrqOwned, 0);
        if (c->L2WasMasked) { BcmWrite(c->L2, 8, 1); (void)BcmRead(c->L2, 4); }
    }
    for (pin = 0; pin < 64; ++pin) if (c->Touched & (1ull << pin)) {
        status = BcmRestore(c, pin, &c->Boot[pin]);
        if (!NT_SUCCESS(status)) result = status;
    }
    return result;
}

static NTSTATUS RestoreFree(BCM_GPIO *c, ULONGLONG Mask)
{
    ULONG pin;
    NTSTATUS status;
    Mask &= ~(c->IoOwned | c->IrqOwned | c->FunctionOwned);
    for (pin = 0; pin < 64; ++pin) if (Mask & (1ull << pin)) {
        status = BcmRestore(c, pin, &c->Boot[pin]);
        if (!NT_SUCCESS(status)) return status;
        c->Touched &= ~(1ull << pin);
    }
    return STATUS_SUCCESS;
}
static NTSTATUS CanClaim(BCM_GPIO *c, ULONGLONG Mask, ULONG Function, UCHAR Pull)
{
    ULONG pin;
    if (!c->Started) return STATUS_DEVICE_NOT_READY;
    if (Pull > GPIO_PIN_PULL_CONFIGURATION_NONE) return STATUS_NOT_SUPPORTED;
    for (pin = 0; pin < 64; ++pin) if (Mask & (1ull << pin)) {
        BCM_PIN_FIELDS fields;
        (void)BcmPinFields(c->Revision, c->Uid, pin, &fields);
        if (fields.PullBit == UINT16_MAX && Pull != GPIO_PIN_PULL_CONFIGURATION_DEFAULT &&
            Pull != GPIO_PIN_PULL_CONFIGURATION_NONE) return STATUS_NOT_SUPPORTED;
        if (c->Boot[pin].Mux != 0 && c->Boot[pin].Mux != Function) return STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS BcmConnect(PVOID Context, PGPIO_CONNECT_IO_PINS_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    ULONGLONG mask;
    ULONG pin;
    NTSTATUS status = BcmMask(c, p->BankId, p->PinNumberTable, p->PinCount, &mask);
    if (!NT_SUCCESS(status)) return status;
    if ((p->ConnectMode != ConnectModeInput && p->ConnectMode != ConnectModeOutput) ||
        p->DriveStrength || p->VendorDataLength) return STATUS_NOT_SUPPORTED;
    status = CanClaim(c, mask, 0, p->PullConfiguration);
    if (!NT_SUCCESS(status)) return status;
    Lock(c);
    if ((mask & (c->IoOwned | c->FunctionOwned)) ||
        (p->ConnectMode == ConnectModeOutput && (mask & c->IrqOwned))) status = STATUS_SHARING_VIOLATION;
    else {
        for (pin = 0; pin < 64; ++pin) if (mask & (1ull << pin))
            BcmConfigure(c, pin, 0, p->ConnectMode == ConnectModeOutput, p->PullConfiguration);
        c->IoOwned |= mask;
        if (p->ConnectMode == ConnectModeOutput) c->OutputOwned |= mask;
    }
    Unlock(c); return status;
}
_Use_decl_annotations_
NTSTATUS BcmDisconnect(PVOID Context, PGPIO_DISCONNECT_IO_PINS_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    ULONGLONG mask;
    NTSTATUS status = BcmMask(c, p->BankId, p->PinNumberTable, p->PinCount, &mask);
    if (!NT_SUCCESS(status)) return status;
    Lock(c);
    c->IoOwned &= ~mask; c->OutputOwned &= ~mask;
    if (!p->DisconnectFlags.PreserveConfiguration) status = RestoreFree(c, mask);
    Unlock(c); return status;
}
_Use_decl_annotations_
NTSTATUS BcmReadPins(PVOID Context, PGPIO_READ_PINS_MASK_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    ULONGLONG data;
    NTSTATUS status;
    if (p->BankId || !c->Started) return STATUS_INVALID_PARAMETER;
    status = BcmReadData(c, &data);
    if (NT_SUCCESS(status)) *p->PinValues = data & c->Valid;
    return status;
}
_Use_decl_annotations_
NTSTATUS BcmWritePins(PVOID Context, PGPIO_WRITE_PINS_MASK_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    if (p->BankId || !c->Started) return STATUS_INVALID_PARAMETER;
    return BcmWriteData(c, p->SetMask, p->ClearMask);
}
_Use_decl_annotations_
NTSTATUS BcmEnableInterrupt(PVOID Context, PGPIO_ENABLE_INTERRUPT_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    ULONGLONG bit;
    NTSTATUS status;
    if (c->Uid) return STATUS_NOT_SUPPORTED;
    status = BcmMask(c, p->BankId, &p->PinNumber, 1, &bit);
    if (!NT_SUCCESS(status)) return status;
    if (p->VendorDataLength) return STATUS_NOT_SUPPORTED;
    status = CanClaim(c, bit, 0, p->PullConfiguration);
    if (!NT_SUCCESS(status)) return status;
    Lock(c);
    if (bit & (c->FunctionOwned | c->OutputOwned | c->IrqOwned)) status = STATUS_SHARING_VIOLATION;
    else {
        status = BcmTrigger(c, p->PinNumber, p->InterruptMode, p->Polarity);
        if (NT_SUCCESS(status)) {
            BcmConfigure(c, p->PinNumber, 0, FALSE, p->PullConfiguration);
            BcmWrite(c->Gio, (p->PinNumber / 32) * 32 + BCM_STATUS, (ULONG)(bit >> ((p->PinNumber / 32) * 32)));
            c->IrqOwned |= bit;
            BcmUpdate(c, BCM_MASK, bit, bit);
            (void)BcmReadBanks(c, BCM_MASK);
        }
    }
    Unlock(c); return status;
}
_Use_decl_annotations_
NTSTATUS BcmDisableInterrupt(PVOID Context, PGPIO_DISABLE_INTERRUPT_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    ULONGLONG bit;
    NTSTATUS status;
    if (c->Uid) return STATUS_NOT_SUPPORTED;
    status = BcmMask(c, p->BankId, &p->PinNumber, 1, &bit);
    if (!NT_SUCCESS(status)) return status;
    Lock(c);
    BcmUpdate(c, BCM_MASK, bit, 0);
    c->IrqOwned &= ~bit;
    status = RestoreFree(c, bit);
    Unlock(c); return status;
}
_Use_decl_annotations_
NTSTATUS BcmMaskInterrupts(PVOID Context, PGPIO_MASK_INTERRUPT_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    p->FailedMask = p->PinMask;
    if (p->BankId || c->Uid || (p->PinMask & ~c->Valid)) return STATUS_INVALID_PARAMETER;
    BcmUpdate(c, BCM_MASK, p->PinMask, 0); (void)BcmReadBanks(c, BCM_MASK);
    p->FailedMask = 0; return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS BcmUnmaskInterrupt(PVOID Context, PGPIO_ENABLE_INTERRUPT_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    ULONGLONG bit;
    NTSTATUS status;
    if (c->Uid) return STATUS_NOT_SUPPORTED;
    status = BcmMask(c, p->BankId, &p->PinNumber, 1, &bit);
    if (!NT_SUCCESS(status)) return status;
    BcmUpdate(c, BCM_MASK, bit, bit); (void)BcmReadBanks(c, BCM_MASK);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS BcmQueryInterrupts(PVOID Context, PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    p->ActiveMask = 0;
    if (p->BankId || c->Uid) return STATUS_INVALID_PARAMETER;
    p->ActiveMask = BcmReadBanks(c, BCM_STATUS) & BcmReadBanks(c, BCM_MASK) & c->Valid & p->EnabledMask;
    if (p->ActiveMask) InterlockedIncrement64(&c->InterruptObservations);
    if (p->ActiveMask & (1ull << 20)) InterlockedIncrement64(&c->ButtonObservations);
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS BcmQueryEnabled(PVOID Context, PGPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    p->EnabledMask = 0;
    if (p->BankId || c->Uid) return STATUS_INVALID_PARAMETER;
    p->EnabledMask = BcmReadBanks(c, BCM_MASK) & c->Valid;
    return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS BcmClearInterrupts(PVOID Context, PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    p->FailedClearMask = p->ClearActiveMask;
    if (p->BankId || c->Uid || (p->ClearActiveMask & ~c->Valid)) return STATUS_INVALID_PARAMETER;
    BcmWrite(c->Gio, BCM_STATUS, (ULONG)p->ClearActiveMask);
    BcmWrite(c->Gio, 32 + BCM_STATUS, (ULONG)(p->ClearActiveMask >> 32));
    (void)BcmReadBanks(c, BCM_STATUS);
    p->FailedClearMask = 0; return STATUS_SUCCESS;
}
_Use_decl_annotations_
NTSTATUS BcmReconfigureInterrupt(PVOID Context, PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    ULONGLONG bit, enabled;
    NTSTATUS status;
    if (c->Uid) return STATUS_NOT_SUPPORTED;
    status = BcmMask(c, p->BankId, &p->PinNumber, 1, &bit);
    if (!NT_SUCCESS(status)) return status;
    enabled = BcmReadBanks(c, BCM_MASK) & bit;
    BcmUpdate(c, BCM_MASK, bit, 0);
    status = BcmTrigger(c, p->PinNumber, p->InterruptMode, p->Polarity);
    BcmUpdate(c, BCM_MASK, bit, enabled); (void)BcmReadBanks(c, BCM_MASK);
    return status;
}
_Use_decl_annotations_
NTSTATUS BcmConnectFunction(PVOID Context, PGPIO_CONNECT_FUNCTION_CONFIG_PINS_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    ULONGLONG mask;
    ULONG pin;
    NTSTATUS status = BcmMask(c, p->BankId, p->PinNumberTable, p->PinCount, &mask);
    if (!NT_SUCCESS(status)) return status;
    if (!p->FunctionNumber || p->FunctionNumber > 8 || p->VendorDataLength) return STATUS_NOT_SUPPORTED;
    status = CanClaim(c, mask, p->FunctionNumber, p->PullConfiguration);
    if (!NT_SUCCESS(status)) return status;
    Lock(c);
    if (mask & (c->IoOwned | c->IrqOwned | c->FunctionOwned)) status = STATUS_SHARING_VIOLATION;
    else {
        for (pin = 0; pin < 64; ++pin) if (mask & (1ull << pin))
            BcmConfigure(c, pin, p->FunctionNumber, FALSE, p->PullConfiguration);
        c->FunctionOwned |= mask;
    }
    Unlock(c); return status;
}
_Use_decl_annotations_
NTSTATUS BcmDisconnectFunction(PVOID Context, PGPIO_DISCONNECT_FUNCTION_CONFIG_PINS_PARAMETERS p)
{
    BCM_GPIO *c = Context;
    ULONGLONG mask;
    NTSTATUS status = BcmMask(c, p->BankId, p->PinNumberTable, p->PinCount, &mask);
    if (!NT_SUCCESS(status)) return status;
    Lock(c); c->FunctionOwned &= ~mask; status = RestoreFree(c, mask); Unlock(c);
    return status;
}

// Read-only diagnostics in the device's registry key. No public arbitrary-MMIO
// or pin-control escape hatch; clients must use their ACPI GPIO connections.
_Use_decl_annotations_
VOID BcmTelemetry(WDFTIMER Timer)
{
    BCM_GPIO *c = BcmTimerContext(Timer)->Controller;
    struct {
        ULONG Version, Uid, Revision, Status;
        ULONGLONG Valid, Data, Direction, IoOwned, IrqOwned, FunctionOwned, Enabled;
        ULONGLONG Interrupts, ButtonInterrupts;
        ULONG MuxPull[12];
    } d;
    DECLARE_CONST_UNICODE_STRING(name, L"BcmGpioDiagnostics");
    if (!c->Started) return;
    RtlZeroMemory(&d, sizeof(d));
    d.Version = 1; d.Uid = c->Uid; d.Revision = c->Revision;
    Lock(c);
    d.Status = BcmReadData(c, &d.Data);
    d.Valid = c->Valid; d.Direction = BcmReadBanks(c, BCM_DIR);
    d.IoOwned = c->IoOwned; d.IrqOwned = c->IrqOwned; d.FunctionOwned = c->FunctionOwned;
    d.Enabled = c->Uid ? 0 : BcmReadBanks(c, BCM_MASK);
    d.Interrupts = (ULONGLONG)c->InterruptObservations;
    d.ButtonInterrupts = (ULONGLONG)c->ButtonObservations;
    BcmReadPinctrl(c, d.MuxPull);
    Unlock(c);
    (void)WdfRegistryAssignValue(c->Key, &name, REG_BINARY, sizeof(d), &d);
    if (c->Started) WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(1000));
}
