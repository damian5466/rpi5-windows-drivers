# VideoCore firmware clocks

`Pi5Fclk.sys` provides leases for the V3D, core and display firmware clocks.
Shared display leases preserve the current display configuration; an exclusive
V3D client can change its GPU clock within firmware limits. The service also
requests the firmware's maximum supported ARM CPU frequency while running and
restores the previous request when stopped. Firmware retains voltage, thermal
and undervoltage control.

It depends on [pi5-mailbox](../pi5-mailbox/README.md) 0.1.0.10 or newer and
matching firmware exposing `ACPI\RPI1030`.
