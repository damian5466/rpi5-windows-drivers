// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "rates.h"
static ULONG Read(PUCHAR registers, ULONG offset)
{ return READ_REGISTER_ULONG((PULONG)(registers + offset)); }

// The board supplies a 50 MHz crystal. Read PLL state; never retune a PLL
// shared with Ethernet, USB, the fan, or the RP1 processor.
static ULONG Pll(PUCHAR registers, ULONG base)
{
    ULONG cs = Read(registers, base), power = Read(registers, base + 4);
    ULONG prim = Read(registers, base + 0x10);
    ULONG ref = cs & 0x3f, d1 = (prim >> 16) & 7, d2 = (prim >> 12) & 7;
    ULONGLONG feedback = (ULONGLONG)(Read(registers, base + 8) & 0xfff) << 24;
    ULONGLONG rate;
    if (!(cs & 0x80000000u) || (power & 0x29) || !ref || !d1 || !d2) return 0;
    if (!(power & 4)) feedback |= Read(registers, base + 12) & 0xffffff;
    rate = 50000000ull * feedback / ((ULONGLONG)ref * d1 * d2 << 24);
    return rate <= 0xffffffffu ? (ULONG)rate : 0;
}
static ULONG ClockRate(PUCHAR Registers, ULONG base, BOOLEAN Gated)
{
    ULONG control = Read(Registers, base), divider = Read(Registers, base + 4), parent = 0;
    // CLK_SYS is a glitchless, always-running clock with no ENABLE field.
    if (Gated && !(control & RP1_CLK_ENABLE)) return 0;
    if (Gated) {
        switch ((control >> 5) & 0x1f) {
        case 0:
            if (Read(Registers, 0x8010) & 0x10) parent = Pll(Registers, 0x8000) / 2;
            break;
        case 1: parent = Pll(Registers, 0x10000); break;
        case 2: parent = 50000000; break;
        default: return 0;
        }
        divider &= 0xff;
    } else {
        switch (control & 3) {
        case 0: parent = 50000000; break;
        case 2: parent = Pll(Registers, 0x8000); break;
        default: return 0;
        }
        divider &= 0xffffff;
    }
    // The RP1 clock driver defines divider zero as 65536.
    return parent / (divider ? divider : 65536);
}
ULONG Rp1ClockRate(PUCHAR Registers, BOOLEAN Uart)
{ return ClockRate(Registers, Uart ? RP1_CLK_UART : RP1_CLK_SYS, Uart); }
ULONG Rp1DmaClockRate(PUCHAR Registers)
{ return ClockRate(Registers, RP1_CLK_DMA, TRUE); }
