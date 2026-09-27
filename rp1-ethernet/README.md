# RP1 Ethernet

`Pi5Ethernet.sys` supplies the Windows NDIS driver for the Pi 5's built-in
Ethernet controller. It manages the MAC, PHY, packet DMA and interrupt-driven
transmit and receive processing.

It depends on [rp1-service](../rp1-service/README.md) and matching firmware
exposing the RP1 Ethernet resources and noncoherent DMA contract.
