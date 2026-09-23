// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"
#include <acpiioct.h>

ULONG BcmRead(PUCHAR Base, ULONG Offset)
{ return READ_REGISTER_ULONG((PULONG)(Base + Offset)); }
VOID BcmWrite(PUCHAR Base, ULONG Offset, ULONG Value)
{ WRITE_REGISTER_ULONG((PULONG)(Base + Offset), Value); }
ULONG BcmFieldRead(BCM_GPIO *c, USHORT Position, ULONG Mask)
{ return (BcmRead(c->Pinctrl, (Position / 32) * 4) >> (Position % 32)) & Mask; }
VOID BcmFieldWrite(BCM_GPIO *c, USHORT Position, ULONG Mask, ULONG Value)
{
    ULONG offset = (Position / 32) * 4, shift = Position % 32;
    BcmWrite(c->Pinctrl, offset, (BcmRead(c->Pinctrl, offset) & ~(Mask << shift)) | ((Value & Mask) << shift));
}
static NTSTATUS Eval(WDFDEVICE Device, PVOID Input, ULONG Size, ULONG *Value)
{
    ACPI_EVAL_OUTPUT_BUFFER output;
    WDF_MEMORY_DESCRIPTOR in, out;
    WDF_REQUEST_SEND_OPTIONS options;
    NTSTATUS status;
    ULONG_PTR returned = 0;
    RtlZeroMemory(&output, sizeof(output));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, Input, Size);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, &output, sizeof(output));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(3));
    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device), NULL,
        IOCTL_ACPI_EVAL_METHOD, &in, &out, &options, &returned);
    if (!NT_SUCCESS(status)) return status;
    if (returned < sizeof(output) || output.Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE ||
        output.Count != 1 || output.Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER ||
        output.Argument[0].DataLength != sizeof(ULONG)) return STATUS_ACPI_INVALID_DATA;
    *Value = output.Argument[0].Argument;
    return STATUS_SUCCESS;
}
NTSTATUS BcmEvalInteger(WDFDEVICE Device, const CHAR Name[4], ULONG *Value)
{
    ACPI_EVAL_INPUT_BUFFER input;
    RtlZeroMemory(&input, sizeof(input));
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(input.MethodName, Name, 4);
    return Eval(Device, &input, sizeof(input), Value);
}
NTSTATUS BcmBroker(BCM_GPIO *c, ULONG Mask, ULONG Value, ULONG *Data)
{
    static const GUID uuid = {0xa95b0d30,0x818e,0x4a96,{0xa4,0x7b,0x72,0x6d,0x07,0x23,0x05,0x03}};
    union { ULONGLONG Align; UCHAR Bytes[128]; } storage;
    PACPI_EVAL_INPUT_BUFFER_COMPLEX input = (PVOID)storage.Bytes;
    PACPI_METHOD_ARGUMENT arg, child;
    ULONG args, bytes;
    NTSTATUS status;
    if (!c->Uid || (Mask & ~(ULONG)c->Valid)) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&storage, sizeof(storage));
    args = ACPI_METHOD_ARGUMENT_LENGTH(sizeof(GUID)) + 2 * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG)) +
        ACPI_METHOD_ARGUMENT_LENGTH(Mask ? 2 * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG)) : 0);
    bytes = FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) + args;
    input->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    RtlCopyMemory(input->MethodName, "_DSM", 4);
    input->Size = args; input->ArgumentCount = 4;
    arg = input->Argument; arg->Type = ACPI_METHOD_ARGUMENT_BUFFER; arg->DataLength = sizeof(uuid);
    RtlCopyMemory(arg->Data, &uuid, sizeof(uuid));
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg); ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, 1);
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg); ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, Mask ? 2 : 1);
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg); arg->Type = ACPI_METHOD_ARGUMENT_PACKAGE_EX;
    arg->DataLength = Mask ? 2 * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG)) : 0;
    if (Mask) {
        child = (PVOID)arg->Data; ACPI_METHOD_SET_ARGUMENT_INTEGER(child, Mask);
        child = ACPI_METHOD_NEXT_ARGUMENT(child); ACPI_METHOD_SET_ARGUMENT_INTEGER(child, Value & Mask);
    }
    status = Eval(c->Device, input, bytes, Data);
    if (NT_SUCCESS(status) && *Data == MAXULONG) return STATUS_ACPI_INVALID_DATA;
    return status;
}
ULONGLONG BcmReadBanks(BCM_GPIO *c, ULONG Register)
{ return BcmRead(c->Gio, Register) | ((ULONGLONG)BcmRead(c->Gio, 32 + Register) << 32); }
VOID BcmUpdate(BCM_GPIO *c, ULONG Register, ULONGLONG Mask, ULONGLONG Value)
{
    ULONG bank;
    // Never use this helper for AON bank-0 DATA: it is AML-owned.
    NT_ASSERT(Register != BCM_DATA);
    for (bank = 0; bank < 2; ++bank) {
        ULONG bits = (ULONG)(Mask >> (bank * 32)), offset = bank * 32 + Register;
        if (bits) BcmWrite(c->Gio, offset,
            (BcmRead(c->Gio, offset) & ~bits) | ((ULONG)(Value >> (bank * 32)) & bits));
    }
}
NTSTATUS BcmReadData(BCM_GPIO *c, ULONGLONG *Data)
{
    ULONG low;
    NTSTATUS status = STATUS_SUCCESS;
    if (c->Uid) status = BcmBroker(c, 0, 0, &low);
    else low = BcmRead(c->Gio, BCM_DATA);
    if (NT_SUCCESS(status)) *Data = low | ((ULONGLONG)BcmRead(c->Gio, 32 + BCM_DATA) << 32);
    return status;
}
NTSTATUS BcmWriteData(BCM_GPIO *c, ULONGLONG Set, ULONGLONG Clear)
{
    ULONGLONG mask = Set | Clear;
    ULONG bank, data;
    NTSTATUS status;
    if ((mask & ~c->Valid) || (Set & Clear)) return STATUS_INVALID_PARAMETER;
    if (c->Uid && (ULONG)mask) {
        status = BcmBroker(c, (ULONG)mask, (ULONG)Set, &data);
        if (!NT_SUCCESS(status)) return status;
    }
    for (bank = c->Uid ? 1u : 0u; bank < 2; ++bank) {
        ULONG bits = (ULONG)(mask >> (bank * 32)), offset = bank * 32 + BCM_DATA;
        if (bits) BcmWrite(c->Gio, offset,
            (BcmRead(c->Gio, offset) & ~bits) | (ULONG)(Set >> (bank * 32)));
    }
    return STATUS_SUCCESS;
}
VOID BcmSnapshot(BCM_GPIO *c, ULONG Pin, ULONGLONG Data, BCM_PIN_STATE *s)
{
    BCM_PIN_FIELDS f;
    ULONG bank = (Pin / 32) * 32, bit = 1u << (Pin % 32);
    (void)BcmPinFields(c->Revision, c->Uid, Pin, &f);
    s->Mux = BcmFieldRead(c, f.MuxBit, 15);
    s->Pull = f.PullBit == UINT16_MAX ? 0 : BcmFieldRead(c, f.PullBit, 3);
    s->Data = (Data & (1ull << Pin)) != 0;
    s->Input = (BcmRead(c->Gio, bank + BCM_DIR) & bit) != 0;
    s->OpenDrain = (BcmRead(c->Gio, bank + BCM_ODEN) & bit) != 0;
    s->Edge = (BcmRead(c->Gio, bank + BCM_EDGE) & bit) != 0;
    s->Both = (BcmRead(c->Gio, bank + BCM_BOTH) & bit) != 0;
    s->Level = (BcmRead(c->Gio, bank + BCM_LEVEL) & bit) != 0;
}
VOID BcmReadPinctrl(BCM_GPIO *c, ULONG Registers[12])
{
    BCM_PIN_FIELDS f;
    ULONG pin, index, used = 0;
    // An ACPI aperture is not a list of readable registers. In particular,
    // D0 main pinctrl has a reserved word at 0x1c. Only read words that hold
    // a documented mux or pull field for an implemented pin.
    for (pin = 0; pin < 64; ++pin) if (c->Valid & (1ull << pin)) {
        (void)BcmPinFields(c->Revision, c->Uid, pin, &f);
        used |= 1u << (f.MuxBit / 32);
        if (f.PullBit != UINT16_MAX) used |= 1u << (f.PullBit / 32);
    }
    for (index = 0; index < 12; ++index)
        Registers[index] = (used & (1u << index)) ? BcmRead(c->Pinctrl, index * 4) : 0;
}
NTSTATUS BcmRestore(BCM_GPIO *c, ULONG Pin, const BCM_PIN_STATE *s)
{
    BCM_PIN_FIELDS f;
    ULONGLONG bit = 1ull << Pin;
    NTSTATUS status;
    BOOLEAN hold;
    (void)BcmPinFields(c->Revision, c->Uid, Pin, &f);
    hold = !s->Input && s->Mux == 0 && BcmFieldRead(c, f.MuxBit, 15) == 0 &&
        !(BcmRead(c->Gio, (Pin / 32) * 32 + BCM_DIR) & (1u << (Pin % 32)));
    if (!hold) BcmUpdate(c, BCM_DIR, bit, bit);
    status = BcmWriteData(c, s->Data ? bit : 0, s->Data ? 0 : bit);
    if (!NT_SUCCESS(status)) return status;
    BcmUpdate(c, BCM_ODEN, bit, s->OpenDrain ? bit : 0);
    BcmFieldWrite(c, f.MuxBit, 15, s->Mux);
    if (f.PullBit != UINT16_MAX) BcmFieldWrite(c, f.PullBit, 3, s->Pull);
    BcmUpdate(c, BCM_EDGE, bit, s->Edge ? bit : 0);
    BcmUpdate(c, BCM_BOTH, bit, s->Both ? bit : 0);
    BcmUpdate(c, BCM_LEVEL, bit, s->Level ? bit : 0);
    BcmUpdate(c, BCM_DIR, bit, s->Input ? bit : 0);
    return STATUS_SUCCESS;
}
VOID BcmConfigure(BCM_GPIO *c, ULONG Pin, ULONG Function, BOOLEAN Output, UCHAR Pull)
{
    BCM_PIN_FIELDS f;
    ULONGLONG bit = 1ull << Pin;
    BOOLEAN hold;
    (void)BcmPinFields(c->Revision, c->Uid, Pin, &f);
    // Keep established board supplies driven while taking GPIO ownership.
    hold = Output && Function == 0 && BcmFieldRead(c, f.MuxBit, 15) == 0 &&
        !(BcmRead(c->Gio, (Pin / 32) * 32 + BCM_DIR) & (1u << (Pin % 32)));
    if (!hold) BcmUpdate(c, BCM_DIR, bit, bit);
    BcmFieldWrite(c, f.MuxBit, 15, Function);
    if (f.PullBit != UINT16_MAX) {
        ULONG value = Pull == GPIO_PIN_PULL_CONFIGURATION_DEFAULT ? c->Boot[Pin].Pull :
            Pull == GPIO_PIN_PULL_CONFIGURATION_PULLUP ? 2 : Pull == GPIO_PIN_PULL_CONFIGURATION_PULLDOWN ? 1 : 0;
        BcmFieldWrite(c, f.PullBit, 3, value);
    }
    BcmUpdate(c, BCM_ODEN, bit, 0);
    if (Output) BcmUpdate(c, BCM_DIR, bit, 0);
    c->Touched |= bit;
}
NTSTATUS BcmMask(BCM_GPIO *c, BANK_ID Bank, const PIN_NUMBER *Pins, ULONG Count, ULONGLONG *Mask)
{
    ULONG i;
    ULONGLONG mask = 0;
    if (Bank || !Pins || !Count || Count > 64) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < Count; ++i) {
        ULONGLONG bit;
        if (Pins[i] >= 64) return STATUS_INVALID_PARAMETER;
        bit = 1ull << Pins[i];
        if (!(c->Valid & bit) || (mask & bit)) return STATUS_INVALID_PARAMETER;
        mask |= bit;
    }
    *Mask = mask;
    return STATUS_SUCCESS;
}
NTSTATUS BcmTrigger(BCM_GPIO *c, ULONG Pin, KINTERRUPT_MODE Mode, KINTERRUPT_POLARITY Polarity)
{
    ULONGLONG bit = 1ull << Pin;
    if ((Mode != Latched && Mode != LevelSensitive) ||
        (Polarity != InterruptActiveLow && Polarity != InterruptActiveHigh &&
         !(Polarity == InterruptActiveBoth && Mode == Latched))) return STATUS_NOT_SUPPORTED;
    BcmUpdate(c, BCM_EDGE, bit, Polarity == InterruptActiveHigh ? bit : 0);
    BcmUpdate(c, BCM_BOTH, bit, Polarity == InterruptActiveBoth ? bit : 0);
    BcmUpdate(c, BCM_LEVEL, bit, Mode == LevelSensitive ? bit : 0);
    return STATUS_SUCCESS;
}
