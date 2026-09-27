# RP1 GPIO and pin control

`Pi5Gpio.sys` provides Windows GpioClx I/O, pin-function selection, pulls and
interrupts for RP1 GPIO. It coordinates pin ownership for the 40-pin header
and board peripherals and restores released pins to their startup state.

It depends on [rp1-service](../rp1-service/README.md),
[rp1-clocks](../rp1-clocks/README.md), and matching firmware GPIO resources.
