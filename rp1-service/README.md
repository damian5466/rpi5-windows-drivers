# RP1 shared interrupt service

`Pi5Rp1.sys` manages the RP1 shared interrupt controller and provides
interrupt-source leases to cooperating RP1 drivers. It coordinates shared
interrupt ownership and device removal.

It requires matching firmware exposing `ACPI\RPI0011` and the RP1 interrupt
service resources.
