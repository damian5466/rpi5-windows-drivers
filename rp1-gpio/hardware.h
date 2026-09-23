// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <ntddk.h>
#include <wdf.h>
#include <gpioclx.h>

#define RP1_HEADER_PINS 28u
// GPIO0/1 are the HAT identification bus. Board-internal GPIO28..53 are
// deliberately not exported by this header controller.
#define RP1_HEADER_MASK 0x0ffffffcu
#define RP1_SET 0x2000u
#define RP1_CLEAR 0x3000u
#define RP1_CTRL(n) (4u + 8u * (n))
#define RP1_PAD(n) (4u + 4u * (n))
#define RP1_INTE 0x11cu
#define RP1_INTS 0x124u
#define RP1_OUT 0u
#define RP1_OE 4u
#define RP1_IN 8u
#define RP1_IRQ_RESET (1u << 28)
#define RP1_IRQ_EVENTS 0x0ff00000u
#define RP1_CONTROL_OVERRIDES 0xc003f000u
#define RP1_PAD_PULL 0x0cu
#define RP1_PAD_IE 0x40u
#define RP1_PAD_OD 0x80u

typedef struct {
    ULONG Control, Pad, Output, Enable;
} RP1_PIN_STATE;
typedef struct {
    WDFDEVICE Device;
    WDFIOTARGET Route;
    PUCHAR Io, Rio, Pads;
    RP1_PIN_STATE Boot[RP1_HEADER_PINS], Resume[RP1_HEADER_PINS];
    ULONG IoOwned, OutputOwned, IrqOwned, FunctionOwned, Touched, ResumeInte;
    BOOLEAN Started;
} GPIO_CONTEXT;

ULONG GpioRead(PUCHAR Base, ULONG Offset);
VOID GpioWrite(PUCHAR Base, ULONG Offset, ULONG Value);
VOID GpioSnapshot(GPIO_CONTEXT *c, ULONG pin, RP1_PIN_STATE *state);
VOID GpioRestore(GPIO_CONTEXT *c, ULONG pin, const RP1_PIN_STATE *state);
NTSTATUS GpioPinMask(BANK_ID Bank, const PIN_NUMBER *Pins, ULONG Count, ULONG *Mask);
NTSTATUS GpioCanClaim(GPIO_CONTEXT *c, ULONG Mask);
VOID GpioConfigure(GPIO_CONTEXT *c, ULONG Pin, ULONG Function, BOOLEAN Output, UCHAR Pull, USHORT Drive);
NTSTATUS GpioTrigger(KINTERRUPT_MODE Mode, KINTERRUPT_POLARITY Polarity, ULONG *Events);

GPIO_CLIENT_PREPARE_CONTROLLER GpioPrepare;
GPIO_CLIENT_RELEASE_CONTROLLER GpioRelease;
GPIO_CLIENT_START_CONTROLLER GpioStart;
GPIO_CLIENT_STOP_CONTROLLER GpioStop;
GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION GpioInformation;
GPIO_CLIENT_QUERY_SET_CONTROLLER_INFORMATION GpioQuerySet;
GPIO_CLIENT_CONNECT_IO_PINS GpioConnect;
GPIO_CLIENT_DISCONNECT_IO_PINS GpioDisconnect;
GPIO_CLIENT_READ_PINS_MASK GpioReadPins;
GPIO_CLIENT_WRITE_PINS_MASK GpioWritePins;
GPIO_CLIENT_ENABLE_INTERRUPT GpioEnableInterrupt;
GPIO_CLIENT_DISABLE_INTERRUPT GpioDisableInterrupt;
GPIO_CLIENT_MASK_INTERRUPTS GpioMaskInterrupts;
GPIO_CLIENT_UNMASK_INTERRUPT GpioUnmaskInterrupt;
GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS GpioQueryInterrupts;
GPIO_CLIENT_QUERY_ENABLED_INTERRUPTS GpioQueryEnabled;
GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS GpioClearInterrupts;
GPIO_CLIENT_RECONFIGURE_INTERRUPT GpioReconfigureInterrupt;
GPIO_CLIENT_CONNECT_FUNCTION_CONFIG_PINS GpioConnectFunction;
GPIO_CLIENT_DISCONNECT_FUNCTION_CONFIG_PINS GpioDisconnectFunction;
