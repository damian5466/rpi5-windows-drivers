# BCM2712 V3D reset gate

`Pi5Pm.sys` provides exclusive leases for the BCM2712 V3D reset gate. It leaves
the legacy GRAFX power register untouched; GPU-local power management belongs
to the GPU consumer.

It requires matching firmware exposing the PM00 resources. Consumers must
also retain a V3D clock lease from [pi5-fclk](../pi5-fclk/README.md).
