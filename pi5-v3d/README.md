# VideoCore VII hardware service

`Pi5V3d` owns the BCM2712 V3D 7.1 GPU resources on `GPU0`. It provides
identification and interrupt diagnostics, coordinated reset, and synchronous
kernel-controlled raster copies through the TFU and fixed 16-lane QPU integer
transforms through CSD. Operations use V3D's internal MMU and guarded,
driver-owned DMA buffers. Compute completion fences include cache writeback
and CPU visibility; shader and uniform mappings are read-only to the GPU.
Administrators can read cached diagnostics; hardware operations require a
kernel client and an exclusive
session. Uncertain completion retains the buffers and provider leases until
reboot.

The service depends on [pi5-graph](../pi5-graph/README.md),
[pi5-fclk](../pi5-fclk/README.md), and [pi5-pm](../pi5-pm/README.md).
Its GPU memory translation is independent of the multimedia IOMMU service.
This is a system hardware service; the Windows desktop uses its existing
display driver.
