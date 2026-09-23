# RP1 package and 40-pin header

RP1 is one driver package with cooperating function drivers. Windows loads
GPIO through GpioClx, UART through SerCx2, and I²C/SPI through SpbCx. Shared
interrupt and clock providers own the common registers. Existing Ethernet and
fan drivers remain part of the package. A future function can use the same
providers without duplicating register ownership or replacing the whole RP1.

The matching firmware supplies translated MMIO/interrupt resources and
`PinFunction` connections. Windows claims peripheral pins when an application
opens a target and returns them to GPIO when the last target closes. Opening
a conflicting GPIO, UART, I²C or SPI function fails. No peripheral pins are
claimed merely by installing these drivers. GPIO0/1 and board-internal pins
remain reserved. Device Manager entries for unsupported internal controllers
can still report that a driver is missing.

## Package installation

Build/sign using the root [build instructions](README.md). Install matching
firmware and the complete header provider set: `rp1-service`, `rp1-clocks`,
`rp1-gpio`, `rp1-uart`, `rp1-i2c`, `rp1-spi`. Firmware's Resource Hub Proxy
includes all header resources, so a missing I²C/SPI provider can also prevent
the user-mode GPIO interface from appearing. This is separate from the native
GPIO controller driver's own PnP status.

From an elevated PowerShell prompt, with trusted signed packages:

```powershell
.\install-rp1.ps1 -PackageRoot .\Build
```

The installer stages all RP1 packages, then installs providers before clients.
It does not enable test signing, change certificate trust, or update firmware.
Windows may require a reboot. Updating binaries while keeping an identical INF
version can leave an existing Driver Store package selected; use a newer
`DriverVer` for a new release. Do not remove a shared provider while its clients
are running.

## Header routes

GPIO numbers and physical header positions are different numbering systems.
All signal pins use 3.3 V logic.

| Function | GPIO signals | Physical pins |
| --- | --- | --- |
| UART0 | TX14, RX15 | TX8, RX10 |
| UART2 | TX4, RX5 | TX7, RX29 |
| UART3 | TX8, RX9 | TX24, RX21 |
| UART4 | TX12, RX13 | TX32, RX33 |
| I²C1, default | SDA2, SCL3 | SDA3, SCL5 |
| I²C0 | SDA8, SCL9 | SDA24, SCL21 |
| I²C2 | SDA4, SCL5 | SDA7, SCL29 |
| I²C3 | SDA6, SCL7 | SDA31, SCL26 |
| SPI0 | MOSI10, MISO9, SCLK11 | MOSI19, MISO21, SCLK23 |
| SPI0 chip selects | CS0=GPIO8, CS1=GPIO7 | CS0=24, CS1=26 |

For the SPI0 MOSI/MISO loopback, physical pins **19 and 21** are adjacent
contacts on the odd-numbered row: the 10th and 11th contacts counting from
physical pin 1. Their GPIO labels are **GPIO10 and GPIO9**. GPIO6 is physical
pin 31. These mappings follow the [official Raspberry Pi pin table](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#pin-gpio-mappings).

For a 3.3 V I²C OLED with labels `GND VCC SCL SDA`, the tested wiring is
GND→physical6, VCC→physical1, SCL→physical5, SDA→physical3. Fit or remove
connections with the Pi shut down and unpowered. Do not connect signal pins to
5 V. I²C needs pull-ups to 3.3 V; other routes need suitable external pull-ups
for their capacitance and speed.

## Applications and limits

Use `Windows.Devices.Gpio`, `Windows.Devices.I2c` and `Windows.Devices.Spi`
from a desktop application. UART exposes standard serial device interfaces
with friendly names `UART0`, `UART2`, `UART3`, `UART4`; these are not legacy
`COMx` names. Each function's README documents its supplied desktop tool.

This is experimental PIO support. It has no DMA, target-mode SPI, 10-bit I²C,
hardware UART flow control, arbitrary pin routing or automatic HAT discovery.
I²C/SPI accept up to 16 transfers and 65,536 aggregate buffer bytes in one
request. Delayed transfers and explicit controller locks spanning separate
requests are rejected. SPI initially exposes SPI0, 8-bit frames, modes 0–3,
100 kHz–4 MHz. Transfer pauses can reduce throughput below the selected wire
clock. Device-specific timing requirements still apply.

Successful OLED writes do not validate reads from a sensor or a write/read
transaction against a responding target. Positive I²C reads and repeated-start
transactions need a suitable test peripheral. There is no HLK/MITT
certification or broad power-management validation yet. Shared-provider
initialization and pin ownership have been exercised across hardware reboots;
arbitrary firmware overlays are not supported by these fixed header routes.

On the tested Windows desktop build, an address NACK returned as the documented
`STATUS_NO_SUCH_DEVICE` reaches WinRT as `UnknownError` / HRESULT `0x800701B1`
(error 433), rather than `SlaveAddressNotAcknowledged`. The supplied I²C tool
recognizes both error 433 and the older error 2 mapping. Data NACKs currently
fail the request; acknowledged partial byte counts are not reported.

## Development validation

Hardware validation uses debug firmware/drivers and a read-only external UART
capture. Firmware output is visible there; this setup does not route Windows
kernel DbgPrint to the probe. Test results come from Windows API calls and
hardware observations, not inferred kernel UART traces. Tests belong outside
the repositories, in the workspace's `tests/rp1-header` directory. Promote a
validated build to release only after the real hardware checks pass.
