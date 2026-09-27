# RP1 header I2C

`Pi5I2c.sys` exposes the firmware-selected RP1 header I2C controllers through
Windows SpbCx. It supports I2C read, write and combined transactions and
coordinates bus pins and controller interrupts.

It depends on [rp1-service](../rp1-service/README.md),
[rp1-clocks](../rp1-clocks/README.md), [rp1-gpio](../rp1-gpio/README.md), and
matching firmware I2C routes.
