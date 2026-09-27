# CYW43455 Bluetooth transport

`Pi5Bluetooth.sys` connects the Pi 5's built-in Bluetooth radio to the Windows
Bluetooth stack through an H4 UART transport. It initializes the radio, loads
its embedded BCM4345C0 firmware, configures the Bluetooth address supplied by
the boot firmware, and carries HCI commands, events and ACL traffic. SCO audio
and vendor radio-control operations are not implemented.

It depends on [bcm2712-gpio](../bcm2712-gpio/README.md),
[bcm2712-uart](../bcm2712-uart/README.md), and matching firmware exposing
`ACPI\RPI1017`.

The transport derives from `worproject/cywbtserialbus` and retains its MS-PL
license in `LICENSE.txt`. The embedded Cypress firmware retains its separate
binary redistribution license in `FIRMWARE-LICENSE.txt`.
