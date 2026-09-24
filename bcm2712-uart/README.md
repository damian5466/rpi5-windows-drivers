# BCM2712 Bluetooth UART

`Pi5BcmUart.sys` binds to `ACPI\RPI1016`. This is the on-board BCM7271-style
8250 UART at CPU address `0x107d50c000`, with 4-byte register spacing and a
32-byte FIFO. It is separate from the RP1 header UART and the debug probe.
The matching firmware supplies its boot-DT clock through `UCLK` and gives
GpioClx ownership of the UART pin-function resources.

The Pi's soldered CYW43455 Bluetooth radio uses BCM2712 GPIO24–27 for
RTS, CTS, TX and RX respectively, and GPIO29 for radio enable. These are
internal SoC GPIO numbers, not 40-pin header numbers. GPIO24 uses function 3
on C0 and function 4 on D0; the other three pins use function 4. Physical
validation has covered D0 only.

The SerCx2 PIO implementation supports hardware CTS, automatic/manual RTS,
a 16 KiB receive ring with software backpressure, and the empty receive-timeout
workaround for this UART. The tested clock is 96 MHz; Bluetooth initialization
uses 115,200 baud, then 460,800 baud with 8N1 and RTS/CTS. Divisors outside a
2% error bound are rejected.

Write completion counts bytes accepted into the FIFO, not bytes acknowledged
by the peripheral. The optional drain/cancel-drain/purge callback trio is
omitted because this aperture has no exact unsent-byte counter. The Bluetooth
client waits for HCI responses before changing baud or resetting the radio.
A timeout or cancellation can return a successful short count; callers must
check that count. `PURGE_TXCLEAR` explicitly resets the transmit FIFO.
See Microsoft's [SerCx2 PIO transmit contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/serports/sercx2-pio-transmit-transactions).
This driver is intended for the resource-hub Bluetooth connection, not as a
general-purpose header COM port.

Build: `.\build.ps1 -Driver bcm2712-uart -Configuration Debug -Analyze`.
Use matching firmware and the BCM2712 GPIO driver before installing it.
Debug builds expose startup and serial counters in the device's
`Device Parameters` registry key. Physical testing includes patch download,
BLE scanning, GATT payloads, and device restarts. System sleep/wake and a C0
board have not been validated.
