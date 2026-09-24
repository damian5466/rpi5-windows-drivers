// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <ntddk.h>
#define RP1_CLK_SYS 0x14u
#define RP1_CLK_UART 0x54u
#define RP1_CLK_DMA 0x44u
#define RP1_CLK_ENABLE (1u << 11)
ULONG Rp1ClockRate(PUCHAR Registers, BOOLEAN Uart);
ULONG Rp1DmaClockRate(PUCHAR Registers);
