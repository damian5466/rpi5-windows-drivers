// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <ntddk.h>
#include <wdf.h>
#include <gpioclx.h>
#include "layout.h"

#define BCM_ODEN 0u
#define BCM_DATA 4u
#define BCM_DIR 8u
#define BCM_EDGE 12u
#define BCM_BOTH 16u
#define BCM_MASK 20u
#define BCM_LEVEL 24u
#define BCM_STATUS 28u
typedef struct {
    ULONG Mux, Pull;
    BOOLEAN Data, Input, OpenDrain, Edge, Both, Level;
} BCM_PIN_STATE;
typedef struct {
    WDFDEVICE Device;
    WDFTIMER Timer;
    WDFKEY Key;
    PUCHAR Gio, Pinctrl, L2;
    ULONG PinctrlLength, Revision, Uid;
    ULONGLONG Valid, IoOwned, OutputOwned, IrqOwned, FunctionOwned, Touched, ResumeMask;
    BCM_PIN_STATE Boot[64], Resume[64];
    volatile LONG64 InterruptObservations, ButtonObservations;
    BOOLEAN Started, L2WasMasked;
} BCM_GPIO;
typedef struct { BCM_GPIO *Controller; } BCM_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BCM_TIMER, BcmTimerContext)

ULONG BcmRead(PUCHAR Base, ULONG Offset);
VOID BcmWrite(PUCHAR Base, ULONG Offset, ULONG Value);
ULONG BcmFieldRead(BCM_GPIO *c, USHORT Position, ULONG Mask);
VOID BcmFieldWrite(BCM_GPIO *c, USHORT Position, ULONG Mask, ULONG Value);
NTSTATUS BcmEvalInteger(WDFDEVICE Device, const CHAR Name[4], ULONG *Value);
NTSTATUS BcmBroker(BCM_GPIO *c, ULONG Mask, ULONG Value, ULONG *Data);
NTSTATUS BcmReadData(BCM_GPIO *c, ULONGLONG *Data);
NTSTATUS BcmWriteData(BCM_GPIO *c, ULONGLONG Set, ULONGLONG Clear);
VOID BcmUpdate(BCM_GPIO *c, ULONG Register, ULONGLONG Mask, ULONGLONG Value);
ULONGLONG BcmReadBanks(BCM_GPIO *c, ULONG Register);
VOID BcmSnapshot(BCM_GPIO *c, ULONG Pin, ULONGLONG Data, BCM_PIN_STATE *State);
VOID BcmReadPinctrl(BCM_GPIO *c, ULONG Registers[12]);
NTSTATUS BcmRestore(BCM_GPIO *c, ULONG Pin, const BCM_PIN_STATE *State);
VOID BcmConfigure(BCM_GPIO *c, ULONG Pin, ULONG Function, BOOLEAN Output, UCHAR Pull);
NTSTATUS BcmMask(BCM_GPIO *c, BANK_ID Bank, const PIN_NUMBER *Pins, ULONG Count, ULONGLONG *Mask);
NTSTATUS BcmTrigger(BCM_GPIO *c, ULONG Pin, KINTERRUPT_MODE Mode, KINTERRUPT_POLARITY Polarity);

GPIO_CLIENT_PREPARE_CONTROLLER BcmPrepare;
GPIO_CLIENT_RELEASE_CONTROLLER BcmRelease;
GPIO_CLIENT_START_CONTROLLER BcmStart;
GPIO_CLIENT_STOP_CONTROLLER BcmStop;
GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION BcmInformation;
GPIO_CLIENT_QUERY_SET_CONTROLLER_INFORMATION BcmQuerySet;
GPIO_CLIENT_CONNECT_IO_PINS BcmConnect;
GPIO_CLIENT_DISCONNECT_IO_PINS BcmDisconnect;
GPIO_CLIENT_READ_PINS_MASK BcmReadPins;
GPIO_CLIENT_WRITE_PINS_MASK BcmWritePins;
GPIO_CLIENT_ENABLE_INTERRUPT BcmEnableInterrupt;
GPIO_CLIENT_DISABLE_INTERRUPT BcmDisableInterrupt;
GPIO_CLIENT_MASK_INTERRUPTS BcmMaskInterrupts;
GPIO_CLIENT_UNMASK_INTERRUPT BcmUnmaskInterrupt;
GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS BcmQueryInterrupts;
GPIO_CLIENT_QUERY_ENABLED_INTERRUPTS BcmQueryEnabled;
GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS BcmClearInterrupts;
GPIO_CLIENT_RECONFIGURE_INTERRUPT BcmReconfigureInterrupt;
GPIO_CLIENT_CONNECT_FUNCTION_CONFIG_PINS BcmConnectFunction;
GPIO_CLIENT_DISCONNECT_FUNCTION_CONFIG_PINS BcmDisconnectFunction;
