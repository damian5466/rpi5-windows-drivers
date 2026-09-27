# RP1 DMA copy service

`Pi5Dma.sys` provides bounded memory-to-memory transfers on RP1's AXI DMA
controller for kernel clients. It uses channel 0, guarded noncached buffers
and interrupt completion, and retains allocations if hardware cannot be
stopped safely. It does not implement peripheral DMA handshakes.

It depends on [rp1-service](../rp1-service/README.md) 0.4.0.0 or newer,
[rp1-clocks](../rp1-clocks/README.md) 0.3.0.0 or newer, and matching firmware
exposing `ACPI\RPI0004`.
