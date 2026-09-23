# RP1 PWM fan driver

Experimental Windows ARM64 driver for the RP1 cooling fan exposed by the
matching firmware with compatible ID `ACPI\RPI00F1`.

See the [repository README](../README.md) for shared build prerequisites and
signing instructions. Run the commands below from the repository root.

## Build

```powershell
.\build.ps1 -Driver rp1-fan -Analyze
```

The package is written to `Build/rp1-fan`, with `Pi5Fan.sys` and `pi5fan.inf`.

## Temperature readings and cooling policy

The fan driver queries `root\wmi:MSAcpi_ThermalZoneTemperature` through the kernel
[`IoWMIOpenBlock`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iowmiopenblock)
and [`IoWMIQueryAllData`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iowmiqueryalldata)
APIs. It has no dependency on the platform driver's header, device interface,
service name or private IOCTL. Any driver publishing the standard class can
supply its readings; no user-mode polling service is required. When multiple
instances exist, the fan uses the hottest plausible reading (-40 to 125 C),
including instances from other providers. Invalid temperatures are ignored;
empty or malformed replies fail the sample. This is a conservative board-wide
policy, not a configurable association between fans and individual sensors.

The fan samples once per second. Its independent 250 ms watchdog maintains
full speed when there is no usable temperature, a reading is older than
2.5 seconds, the fan stalls, or temperature reaches 75 C. Query time counts
toward sample age, so a slow provider cannot make an old reading appear fresh.
The existing automatic curve, hysteresis, startup spin-up and manual lease
protections remain in place. Kernel WMI queries are synchronous and have no
caller-specified timeout; the watchdog stays independent, but a stuck provider
can delay fan device shutdown while the sensor callback finishes.

Install the [platform package](../bcm2712-platform/README.md) before the new fan
package, otherwise fan will run at maximum speed.

## Tests

The parser/policy tests run on an x64 Windows build host with the same SDK/WDK:

```powershell
.\tests\test-thermal.ps1
# Or use the same extracted WDK as the driver build:
.\tests\test-thermal.ps1 -KernelKit C:\Kits\Kernel
```

They exercise chained providers, fixed and variable instance sizes, WMI
alignment, malformed/truncated buffers, temperature conversion, hottest-sensor
selection, and stale/missing/hot temperature fail-safes.
