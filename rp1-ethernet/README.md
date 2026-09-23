# RP1 Ethernet driver

Experimental Windows ARM64 NDIS 6.30 miniport for the RP1 Ethernet controller
exposed by the matching firmware as `ACPI\RPI0001`.

See the [repository README](../README.md) for shared build prerequisites and
signing instructions. Run the commands below from the repository root.

## Build

```powershell
.\build.ps1 -Driver rp1-ethernet -Analyze
```

The package is written to `Build/rp1-ethernet`, with `Pi5Ethernet.sys` and
`pi5ethernet.inf`.
