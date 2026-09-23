// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"

ULONG GpioRead(PUCHAR Base, ULONG Offset)
{ return READ_REGISTER_ULONG((PULONG)(Base + Offset)); }
VOID GpioWrite(PUCHAR Base, ULONG Offset, ULONG Value)
{ WRITE_REGISTER_ULONG((PULONG)(Base + Offset), Value); }
static ULONG BankOffset(ULONG pin) { return pin < 28 ? 0 : pin < 34 ? 0x4000 : 0x8000; }
static ULONG BankPin(ULONG pin) { return pin < 28 ? pin : pin < 34 ? pin - 28 : pin - 34; }
VOID GpioSnapshot(GPIO_CONTEXT *c, ULONG pin, RP1_PIN_STATE *state)
{
    ULONG bank = BankOffset(pin), local = BankPin(pin), bit = 1u << local;
    state->Control = GpioRead(c->Io + bank, RP1_CTRL(local)) & ~RP1_IRQ_RESET;
    state->Pad = GpioRead(c->Pads + bank, RP1_PAD(local));
    state->Output = GpioRead(c->Rio + bank, RP1_OUT) & bit;
    state->Enable = GpioRead(c->Rio + bank, RP1_OE) & bit;
}
VOID GpioRestore(GPIO_CONTEXT *c, ULONG pin, const RP1_PIN_STATE *state)
{
    ULONG bank = BankOffset(pin), local = BankPin(pin), bit = 1u << local;
    PUCHAR io = c->Io + bank, rio = c->Rio + bank, pads = c->Pads + bank;
    BOOLEAN hold = pin >= RP1_HEADER_PINS && state->Enable && !(state->Pad & RP1_PAD_OD) &&
        (state->Control & (31u | RP1_CONTROL_OVERRIDES)) == 5 &&
        (GpioRead(io, RP1_CTRL(local)) & (31u | RP1_CONTROL_OVERRIDES)) == 5 &&
        (GpioRead(rio, RP1_OE) & bit) && !(GpioRead(pads, RP1_PAD(local)) & RP1_PAD_OD);
    // A live board output (notably PHY reset) must not float during restore.
    if (!hold) {
        GpioWrite(pads, RP1_SET + RP1_PAD(local), RP1_PAD_OD);
        GpioWrite(rio, RP1_CLEAR + RP1_OE, bit);
    }
    GpioWrite(rio, (state->Output ? RP1_SET : RP1_CLEAR) + RP1_OUT, bit);
    GpioWrite(io, RP1_CTRL(local), state->Control);
    GpioWrite(rio, (state->Enable ? RP1_SET : RP1_CLEAR) + RP1_OE, bit);
    GpioWrite(pads, RP1_PAD(local), state->Pad);
    (void)GpioRead(pads, RP1_PAD(local));
}
NTSTATUS GpioPinMask(BANK_ID Bank, const PIN_NUMBER *Pins, ULONG Count, ULONGLONG *Mask)
{
    ULONG i;
    ULONGLONG mask = 0;
    if (Bank || !Pins || !Count || Count > RP1_TOTAL_PINS) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < Count; ++i) {
        ULONGLONG bit;
        if (Pins[i] >= RP1_TOTAL_PINS) return STATUS_INVALID_PARAMETER;
        bit = 1ull << Pins[i];
        if (!(RP1_ALLOWED_MASK & bit) || (mask & bit)) return STATUS_INVALID_PARAMETER;
        mask |= bit;
    }
    *Mask = mask;
    return STATUS_SUCCESS;
}
NTSTATUS GpioIrqPinMask(BANK_ID Bank, PIN_NUMBER Pin, ULONG *Mask)
{
    if (Bank || Pin < 2 || Pin >= RP1_HEADER_PINS) return STATUS_INVALID_PARAMETER;
    *Mask = 1u << Pin;
    return STATUS_SUCCESS;
}
NTSTATUS GpioCanClaim(GPIO_CONTEXT *c, ULONGLONG Mask)
{
    ULONG pin;
    if (!c->Started) return STATUS_DEVICE_NOT_READY;
    if (Mask & ~RP1_ALLOWED_MASK) return STATUS_INVALID_PARAMETER;
    for (pin = 2; pin < RP1_TOTAL_PINS; ++pin) if (Mask & (1ull << pin)) {
        ULONG function = c->Boot[pin].Control & 31;
        // Firmware-selected peripheral functions are not an implicit handoff.
        if (function != 5 && function != 31) return STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}
ULONGLONG GpioReadValues(GPIO_CONTEXT *c, BOOLEAN Output)
{
    ULONG offset = Output ? RP1_OUT : RP1_IN;
    return (GpioRead(c->Rio, offset) & RP1_HEADER_MASK) |
        ((ULONGLONG)(GpioRead(c->Rio + 0x4000, offset) & (1u << 4)) << 28) |
        ((ULONGLONG)(GpioRead(c->Rio + 0x8000, offset) & ((1u << 0) | (1u << 10) | (1u << 12))) << 34);
}
VOID GpioWriteValues(GPIO_CONTEXT *c, ULONGLONG Set, ULONGLONG Clear)
{
    static const ULONG starts[] = {0,28,34};
    static const ULONG masks[] = {RP1_HEADER_MASK,1u << 4,(1u << 0) | (1u << 10) | (1u << 12)};
    ULONG bank;
    for (bank = 0; bank < 3; ++bank) {
        ULONG set = (ULONG)(Set >> starts[bank]) & masks[bank];
        ULONG clear = (ULONG)(Clear >> starts[bank]) & masks[bank];
        if (!(set | clear)) continue;
        GpioWrite(c->Rio + bank * 0x4000, RP1_CLEAR + RP1_OUT, clear);
        GpioWrite(c->Rio + bank * 0x4000, RP1_SET + RP1_OUT, set);
        (void)GpioRead(c->Rio + bank * 0x4000, RP1_OUT);
    }
}
VOID GpioConfigure(GPIO_CONTEXT *c, ULONG Pin, ULONG Function, BOOLEAN Output, UCHAR Pull, USHORT Drive)
{
    ULONG bank = BankOffset(Pin), local = BankPin(Pin), bit = 1u << local;
    ULONGLONG logical = 1ull << Pin;
    PUCHAR io = c->Io + bank, rio = c->Rio + bank, pads = c->Pads + bank;
    ULONG ctrl = GpioRead(io, RP1_CTRL(local));
    ULONG pad = GpioRead(pads, RP1_PAD(local));
    BOOLEAN hold = Pin >= RP1_HEADER_PINS && Output && Function == 5 &&
        (ctrl & (31u | RP1_CONTROL_OVERRIDES)) == 5 &&
        (GpioRead(rio, RP1_OE) & bit) && !(pad & RP1_PAD_OD);
    if (!hold) {
        GpioWrite(pads, RP1_SET + RP1_PAD(local), RP1_PAD_OD);
        GpioWrite(rio, RP1_CLEAR + RP1_OE, bit);
    }
    ctrl &= ~(31u | RP1_CONTROL_OVERRIDES | RP1_IRQ_RESET);
    if (!(c->IrqOwned & logical)) ctrl &= ~RP1_IRQ_EVENTS;
    ctrl |= Function;
    pad = (pad & ~RP1_PAD_PULL) | RP1_PAD_IE;
    if (Pull == GPIO_PIN_PULL_CONFIGURATION_DEFAULT) pad |= c->Boot[Pin].Pad & RP1_PAD_PULL;
    else if (Pull == GPIO_PIN_PULL_CONFIGURATION_PULLUP) pad |= 8;
    else if (Pull == GPIO_PIN_PULL_CONFIGURATION_PULLDOWN) pad |= 4;
    if (Drive) pad = (pad & ~0x30u) | ((Drive == 200 ? 0u : Drive == 400 ? 1u : Drive == 800 ? 2u : 3u) << 4);
    if (Output || Function != 5) pad &= ~RP1_PAD_OD;
    else pad |= RP1_PAD_OD;
    GpioWrite(io, RP1_CTRL(local), ctrl);
    if (Output) GpioWrite(rio, RP1_SET + RP1_OE, bit);
    GpioWrite(pads, RP1_PAD(local), pad);
    (void)GpioRead(pads, RP1_PAD(local));
    c->Touched |= logical;
}
NTSTATUS GpioTrigger(KINTERRUPT_MODE Mode, KINTERRUPT_POLARITY Polarity, ULONG *Events)
{
    if (Mode == Latched) {
        if (Polarity == InterruptActiveHigh) *Events = 1u << 21;
        else if (Polarity == InterruptActiveLow) *Events = 1u << 20;
        else if (Polarity == InterruptActiveBoth) *Events = 3u << 20;
        else return STATUS_NOT_SUPPORTED;
    } else if (Mode == LevelSensitive) {
        if (Polarity == InterruptActiveHigh) *Events = 1u << 23;
        else if (Polarity == InterruptActiveLow) *Events = 1u << 22;
        else return STATUS_NOT_SUPPORTED;
    } else return STATUS_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}
