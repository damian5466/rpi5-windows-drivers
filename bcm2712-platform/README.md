# BCM2712 temperature and RNG

`Pi5Platform.sys` provides the BCM2712 temperature sensor and hardware random
number generator. It publishes live temperature through Windows' standard
`MSAcpi_ThermalZoneTemperature` WMI class for monitoring applications and the
[fan driver](../rp1-fan/README.md).

It requires matching firmware exposing `ACPI\RPI1014` and `ACPI\RPI1015`.
