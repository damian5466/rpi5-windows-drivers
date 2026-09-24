# CYW43455 Bluetooth transport (work in progress)

ACPI RPI1017 uses the BCM2712 UART (RPI1016), RTS/CTS on GPIO24–27, and
radio enable/reset GPIO29 through GpioClx. These are the Pi's internal
BCM2712 GPIO numbers, not physical header pins; no external Bluetooth module
is required. Windows uses its inbox BthMini / BthPort stack above the H4
transport child. Initialization runs at 115200 baud and switches to 460800.

Transport framework derived from https://github.com/worproject/cywbtserialbus
at b9301327a566f0cc037258a326f5c8082890eca5. This directory's driver code is
MS-PL, with the full license in LICENSE.txt; it is an exception to the parent
repository's default license. Original attribution is retained in source.
The Pi 5 vendor initialization resets the radio, validates the BCM4345C0 model,
loads the bundled patch, and sets the boot firmware's Bluetooth address.
RTS is deasserted across the GPIO29 reset sequence to avoid the controller's
download boot mode. The driver verifies all patch commands, firmware build,
address and post-baud communication. Firmware `BMAC` provides a bounded
six-byte boot identity; no runtime traversal of the platform graph is needed.
Idle/wake, reset recovery, SCO audio and vendor radio-control IOCTLs are not
implemented/validated during bring-up. BTHX's required SCO-channel capability
field does not imply that headset audio has been implemented.

The debug transport starts the inbox Bluetooth nodes, loads all 323 firmware
commands, and scans BLE advertisements. Unpaired GATT echo tests passed 20
round trips with 1, 20, 64, 244 and 512-byte values, both with a short-packet
peer and with 251-byte / 2120-microsecond per-link transmit settings on the
workstation. The driver forwards HCI commands unchanged; it does not inject
data-length commands or filter the controller's capabilities.

Build: `.\build.ps1 -Driver cyw-bluetooth -Configuration Debug -Analyze`.
It requires matching firmware, `bcm2712-gpio` and `bcm2712-uart`. The build
embeds the firmware below and generates WPP metadata using the WDK.

BCM4345C0.hcd is from RPi-Distro/bluez-firmware commit
cdf61dc691a49ff01a124752bd04194907f0f9cd, debian/firmware/broadcom.
It is embedded unchanged at build time and retains its separate Cypress
binary license in FIRMWARE-LICENSE.txt. Both licenses accompany the package.
SHA-256: 51c45e77ddad91a19e96dc8fb75295b2087c279940df2634b23baf71b6dea42c
