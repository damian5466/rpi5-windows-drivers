# Pi 5 V3D reset gate provider

`Pi5Pm` owns the BCM2712 PM00 register resource and provides a leased V3D
reset gate at PM+0x304. It leaves the legacy GRAFX register untouched. A V3D
consumer also depends on `Pi5Fclk` for firmware clock 5 and must hold the
clock while using the reset gate. V3D 7.1 SMS power management belongs to
the GPU consumer.
