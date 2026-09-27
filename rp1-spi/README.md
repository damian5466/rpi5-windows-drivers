# RP1 header SPI

`Pi5Spi.sys` exposes RP1 SPI0 through Windows SpbCx. It provides SPI transfers,
clock polarity and phase selection, and chip-select control on the
firmware-selected header route.

It depends on [rp1-service](../rp1-service/README.md),
[rp1-clocks](../rp1-clocks/README.md), [rp1-gpio](../rp1-gpio/README.md), and
matching firmware SPI resources.
