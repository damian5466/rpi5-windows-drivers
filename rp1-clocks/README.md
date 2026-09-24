# RP1 clock service

`Pi5Rp1Clock.sys` binds to `ACPI\RPI0003`. It reads the existing system clock
rate for I²C/SPI and holds a shared UART-clock lease for serial controllers.
On tested firmware the rates are SYS=200 MHz and UART=50 MHz. These are read
from registers rather than assumed in the controller drivers.

An already-running UART clock keeps its configuration. If it is disabled,
the first UART lease selects the 50 MHz crystal, enables it, and the last
lease restores the prior configuration. No PLL is retuned and the Ethernet,
USB and fan clocks are preserved. Kernel-client definitions and the read-only
admin/system status interface are in `common/rp1-clock.h`.

Version 0.3.0.0 adds a separate kernel-only DMA-clock lease. An enabled gate
must have a valid rate at or below 100 MHz and retains its configuration.
A disabled gate temporarily uses the 50 MHz crystal and is restored when the
last DMA lease closes. UART and DMA leases are tracked independently; either kind of active lease
prevents provider removal. The original status
query ABI remains unchanged. The tested Pi's existing DMA clock is 100 MHz.

Build: `.\build.ps1 -Driver rp1-clocks -Configuration Debug -Analyze`.
Install with the [RP1 package](../RP1-HEADER.md).
