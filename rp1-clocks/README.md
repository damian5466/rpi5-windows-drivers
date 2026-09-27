# RP1 clock service

`Pi5Rp1Clock.sys` provides clock-rate queries and clock leases for RP1
peripherals. It preserves firmware-owned clock configuration and tracks
consumers while their peripheral clocks are required.

It requires matching firmware exposing `ACPI\RPI0003` and the RP1 clock
resources.
