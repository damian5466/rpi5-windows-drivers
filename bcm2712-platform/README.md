# BCM2712 temperature and RNG driver

Experimental Windows ARM64 driver for the BCM2712 temperature monitor
(`ACPI\RPI1015`) and hardware random number generator (`ACPI\RPI1014`).

See the [repository README](../README.md) for shared build prerequisites and
signing instructions. Run the commands below from the repository root.

## Build

```powershell
.\build.ps1 -Driver bcm2712-platform -Analyze
```

The package is written to `Build/bcm2712-platform`, with `Pi5Platform.sys` and
`pi5platform.inf`.

Version 0.3 selects RNG versus temperature through the INF's `DeviceKind`
hardware-key value (1/2). It accepts the existing firmware IDs, including the
new revision-qualified variants, without parsing the hardware-ID list. Install
the SYS and INF from the same package; the MMIO validation and public/WMI
interfaces are unchanged.

## Temperature monitoring

The platform driver publishes the BCM2712 sensor through Windows' built-in
`root\wmi:MSAcpi_ThermalZoneTemperature` class. Applications that already read
this class need no Pi-specific headers, IOCTLs, plugins or rebuilds. For example,
Windows PowerShell can read the live temperature directly:

```powershell
Get-CimInstance -Namespace root/wmi -ClassName MSAcpi_ThermalZoneTemperature |
    Select-Object InstanceName, @{Name='Celsius'; Expression={$_.CurrentTemperature / 10.0 - 273.15}}
```

`CurrentTemperature` uses tenths of a kelvin, rounded to the nearest 0.1 K.
The sensor instance is identified by its PnP device (`ACPI\RPI1015\0_0`).
Each query reads the hardware; stopped devices and invalid sensor data return
an error instead of a cached temperature. The RNG does not publish a thermal
instance. The existing private platform API remains available for diagnostics
and RNG clients.

This provides compatibility with **WMI-capable** monitoring software. It does
not add support to applications that only probe vendor-specific CPU registers,
nor does it implement the Windows Sensor API. The WMI instance is a read-only
monitor: it does not create an ACPI thermal zone or install an OS throttling or
shutdown policy. Unimplemented trip points and thermal constants are zero.

The [fan driver](../rp1-fan/README.md) consumes these standard WMI readings;
its README describes cooling policy, upgrade order and tests.
