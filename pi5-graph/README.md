# Pi 5 hardware connection graph

`Pi5Graph.sys` binds `ACPI\RPI1040` and makes the firmware's boot hardware
description available to cooperating Windows drivers. It reads the documented
graph `_DSM` through Acpi.sys, validates a complete snapshot at PnP start, and
serves immutable data from a bounded cache. It does not map hardware, enumerate
new devices, change clocks, or request mailbox ownership.

Resource addresses and interrupts still come from each consumer's translated
PnP resources. Graph `reg`, DMA ranges, reserved memory and ACPI owner strings
are descriptive data, not authorization to access or allocate those resources.
Disabled nodes and metadata nodes are retained, as are empty properties and
unmapped owners. In particular, `/__symbols__` and `/aliases` contain paths,
not ordinary device reference properties.

## Cooperating-driver interface, version 1

[common/pi5-graph.h](../common/pi5-graph.h) defines pointer-free buffered IOCTLs.
Open `\Device\Pi5Graph` from a kernel client at PASSIVE_LEVEL with GENERIC_READ
and read/write sharing. Administrators and SYSTEM can use `\\.\Pi5Graph`.
All requests require FILE_READ_ACCESS. There are no write operations or raw
ACPI evaluation requests. Include the appropriate Windows IOCTL definitions
before the public header (`ntddk.h` in a driver, `winioctl.h` in a desktop tool).

| IOCTL | Input | Output |
| --- | --- | --- |
| QUERY | None | `PI5_GRAPH_INFO`: schema, layout, node/property counts and cache size |
| NODE | `PI5_GRAPH_KEY`: node index; other fields zero | `PI5_GRAPH_NODE`: path, parent, owner, phandle and property count |
| PROPERTY | `PI5_GRAPH_KEY`: node, property index, byte offset | `PI5_GRAPH_PROPERTY`: name, total size, up to 4096 original bytes |
| FIND | `PI5_GRAPH_FIND`: kind, starting index and text or phandle | Matching `PI5_GRAPH_NODE` |
| RESOLVE | `PI5_GRAPH_RESOLVE`: node, property, provider cell-count property and specifier index | `PI5_GRAPH_REFERENCE`: provider index, phandle and decoded cells |

Supply the exact input structure size, Version 1, and at least the complete
output structure size. Numbers in the API are little endian; raw property bytes
retain big-endian DT cells. Strings are NUL terminated. PROPERTY accepts an
offset equal to the property length and returns Length 0; larger offsets fail.
Outputs are initialized completely, including unused data and padding.

FIND supports exact path, exact ACPI owner, compatible-list member and phandle.
Grouped owners can match multiple nodes: repeat with Start = previous index + 1.
RESOLVE decodes each provider's `#*-cells` rather than assuming a fixed width.
An empty CellsProperty selects a plain phandle list, e.g. `firmware` or `cache`.
The caller must select the correct binding. Zero phandles count as empty entries
and return not found when selected. Unknown providers, missing/malformed cell
counts, truncated lists and more than 16 cells per specifier fail. The entire
property is validated before returning a reference, including trailing entries.
Absence returns STATUS_NOT_FOUND; malformed data returns STATUS_DEVICE_DATA_ERROR.

A lookup does not acquire the referenced driver, establish a power dependency,
lease a resource, or activate a disabled device. Consumers must handle provider
absence/removal, open the actual service and respect its ownership contract.
This first version has no integrated GPU kernel consumer or notification API.

## Bounds and validation

The cache admits at most 32,640 nodes, 32,640 properties in total, depth 64,
1024-byte paths/owners including NUL, 256-byte property names including NUL,
and 16 MiB of total cache allocations. These are explicit provider limits; an
oversized graph fails start instead of publishing a partial snapshot. Every
legal graph-method response fits the 8192-byte transport buffer. Unexpected
overflow fails closed. Each ACPI request has a two-second timeout; the load
checks a 30-second deadline before each request. All cache access is passive
level and coordinated by the power-managed KMDF queue and PnP lifecycle.

Tests outside the repository replay the installed AML and synthetic multi-chunk
graphs through the portable parser with ASan/UBSan, fault injection and malformed
responses. ARM64 `/W4 /WX /analyze` builds and live debug tests cover a complete
byte-for-byte graph round trip, typed references, access/buffer rejection, PnP
restart and reboot. Windows DbgPrint is not routed to the attached UART; load
diagnostics also appear under the device's `Device Parameters` registry key.
Read-only UART captures the boot, while PnP, registry, IOCTL results and System
events verify the driver. Suspend/resume and a consumer holding an open kernel
target across removal have not been validated.

The implementation follows the matching firmware's
[ACPI contract](https://github.com/damian5466/rpi5-uefi/blob/master/edk2-platforms/Platform/RaspberryPi/RPi5/ACPI-CONTRACT.md)
and Microsoft's documented
[ACPI method evaluation](https://learn.microsoft.com/en-us/windows-hardware/drivers/acpi/evaluating-acpi-control-methods-synchronously).
