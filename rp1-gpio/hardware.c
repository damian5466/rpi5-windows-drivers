// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "hardware.h"

ULONG GpioRead(PUCHAR Base, ULONG Offset)
{ return READ_REGISTER_ULONG((PULONG)(Base + Offset)); }
VOID GpioWrite(PUCHAR Base, ULONG Offset, ULONG Value)
{ WRITE_REGISTER_ULONG((PULONG)(Base + Offset), Value); }
VOID GpioSnapshot(GPIO_CONTEXT *c, ULONG pin, RP1_PIN_STATE *state)
{
    state->Control = GpioRead(c->Io, RP1_CTRL(pin)) & ~RP1_IRQ_RESET;
    state->Pad = GpioRead(c->Pads, RP1_PAD(pin));
    state->Output = GpioRead(c->Rio, RP1_OUT) & (1u << pin);
    state->Enable = GpioRead(c->Rio, RP1_OE) & (1u << pin);
}
VOID GpioRestore(GPIO_CONTEXT *c, ULONG pin, const RP1_PIN_STATE *state)
{
    ULONG bit = 1u << pin;
    // Disable the pad while restoring the output latch, mux and direction.
    GpioWrite(c->Pads, RP1_SET + RP1_PAD(pin), RP1_PAD_OD);
    GpioWrite(c->Rio, RP1_CLEAR + RP1_OE, bit);
    GpioWrite(c->Rio, (state->Output ? RP1_SET : RP1_CLEAR) + RP1_OUT, bit);
    GpioWrite(c->Io, RP1_CTRL(pin), state->Control);
    GpioWrite(c->Rio, (state->Enable ? RP1_SET : RP1_CLEAR) + RP1_OE, bit);
    GpioWrite(c->Pads, RP1_PAD(pin), state->Pad);
    (void)GpioRead(c->Pads, RP1_PAD(pin));
}
NTSTATUS GpioPinMask(BANK_ID Bank, const PIN_NUMBER *Pins, ULONG Count, ULONG *Mask)
{
    ULONG i, mask = 0;
    if (Bank || !Pins || !Count || Count > RP1_HEADER_PINS) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < Count; ++i) {
        ULONG bit;
        if (Pins[i] < 2 || Pins[i] >= RP1_HEADER_PINS) return STATUS_INVALID_PARAMETER;
        bit = 1u << Pins[i];
        if (mask & bit) return STATUS_INVALID_PARAMETER;
        mask |= bit;
    }
    *Mask = mask;
    return STATUS_SUCCESS;
}
NTSTATUS GpioCanClaim(GPIO_CONTEXT *c, ULONG Mask)
{
    ULONG pin;
    if (!c->Started) return STATUS_DEVICE_NOT_READY;
    if (Mask & ~RP1_HEADER_MASK) return STATUS_INVALID_PARAMETER;
    for (pin = 2; pin < RP1_HEADER_PINS; ++pin) if (Mask & (1u << pin)) {
        ULONG function = c->Boot[pin].Control & 31;
        // Firmware-selected peripheral functions are not an implicit handoff.
        if (function != 5 && function != 31) return STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}
VOID GpioConfigure(GPIO_CONTEXT *c, ULONG Pin, ULONG Function, BOOLEAN Output, UCHAR Pull, USHORT Drive)
{
    ULONG bit = 1u << Pin;
    ULONG ctrl = GpioRead(c->Io, RP1_CTRL(Pin));
    ULONG pad = GpioRead(c->Pads, RP1_PAD(Pin));
    GpioWrite(c->Pads, RP1_SET + RP1_PAD(Pin), RP1_PAD_OD);
    GpioWrite(c->Rio, RP1_CLEAR + RP1_OE, bit);
    ctrl &= ~(31u | RP1_CONTROL_OVERRIDES | RP1_IRQ_RESET);
    if (!(c->IrqOwned & bit)) ctrl &= ~RP1_IRQ_EVENTS;
    ctrl |= Function;
    pad = (pad & ~RP1_PAD_PULL) | RP1_PAD_IE;
    if (Pull == GPIO_PIN_PULL_CONFIGURATION_DEFAULT) pad |= c->Boot[Pin].Pad & RP1_PAD_PULL;
    else if (Pull == GPIO_PIN_PULL_CONFIGURATION_PULLUP) pad |= 8;
    else if (Pull == GPIO_PIN_PULL_CONFIGURATION_PULLDOWN) pad |= 4;
    if (Drive) pad = (pad & ~0x30u) | ((Drive == 200 ? 0u : Drive == 400 ? 1u : Drive == 800 ? 2u : 3u) << 4);
    if (Output || Function != 5) pad &= ~RP1_PAD_OD;
    else pad |= RP1_PAD_OD;
    GpioWrite(c->Io, RP1_CTRL(Pin), ctrl);
    if (Output) GpioWrite(c->Rio, RP1_SET + RP1_OE, bit);
    GpioWrite(c->Pads, RP1_PAD(Pin), pad);
    (void)GpioRead(c->Pads, RP1_PAD(Pin));
    c->Touched |= bit;
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
