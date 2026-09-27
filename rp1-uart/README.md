# RP1 header UART

`Pi5Uart.sys` provides SerCx2 serial I/O for the RP1 header UARTs exposed by
firmware. It supports buffered receive, transmit, flow control and bounded
cancellation through the Windows serial resource interface.

It depends on [rp1-service](../rp1-service/README.md),
[rp1-clocks](../rp1-clocks/README.md), [rp1-gpio](../rp1-gpio/README.md), and
matching firmware UART routes.
