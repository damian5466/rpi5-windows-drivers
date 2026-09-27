# BCM2712 Bluetooth UART

`Pi5BcmUart.sys` provides the on-board UART used by the CYW43455 Bluetooth
radio. It implements SerCx2 serial I/O with RTS/CTS flow control and is
separate from the RP1 header UARTs and debug UART.

It depends on [bcm2712-gpio](../bcm2712-gpio/README.md) and matching firmware
exposing `ACPI\RPI1016` and its clock and pin resources.
