# VideoCore VII hardware service

`Pi5V3d.sys` owns the VideoCore VII GPU resources and provides coordinated
reset, interrupts, GPU memory mappings, texture-format conversion, raster
copies and QPU compute submission to kernel clients. It maintains provider
power dependencies and retains DMA resources until GPU access has retired.
The [graphics driver](../pi5-graphics/README.md) uses this service for rendering.

It depends on [pi5-graph](../pi5-graph/README.md),
[pi5-fclk](../pi5-fclk/README.md), [pi5-pm](../pi5-pm/README.md), and matching
firmware exposing `ACPI\RPI1000`.
