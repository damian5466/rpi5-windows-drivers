# Pi 5 hardware connection graph

`Pi5Graph.sys` exposes the firmware's immutable boot hardware description to
cooperating Windows drivers. It provides node and property lookup and resolves
references between devices. Resource ownership remains with each device's
Plug and Play resources and its driver.

It depends on matching firmware exposing `ACPI\RPI1040` and the hardware
graph ACPI interface.
