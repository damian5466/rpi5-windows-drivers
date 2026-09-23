# RP1 shared interrupt service

`Pi5Rp1.sys` binds to `ACPI\RPI0011` and owns the PCIe interrupt-forwarding
configuration. Clients obtain a kernel-only per-file lease for an allowed
local interrupt source; closing the file releases it. The provider refuses an
already-owned/enabled source and never changes the firmware's USB routes.
All clients still handle their own hardware interrupt status on the shared GIC
interrupt. See [package architecture](../RP1-HEADER.md).

The driver maps only the interrupt configuration page from its translated
resources. It does not reset RP1, claim SRAM, alter MIP, or enable every device.
The admin/system-only `\\.\Pi5Rp1` endpoint permits a read-only status query;
user-mode callers cannot acquire interrupt routes. The private cooperating
kernel-client ABI is in `common/rp1-service.h`.

Build: `.\build.ps1 -Driver rp1-service -Configuration Debug -Analyze`.
