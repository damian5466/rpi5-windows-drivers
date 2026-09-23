# RP1 UART

`Pi5Uart.sys` uses SerCx2 and the RP1 PL011 FIFOs. Firmware's `RPI0070` compatible
ID selects header UART0/2/3/4. UART1 uses reserved HAT pins and UART5 is internal;
neither is bound. Routes are listed in the [header guide](../RP1-HEADER.md).

Ports appear as standard `GUID_DEVINTERFACE_COMPORT` interfaces named `UART0`,
`UART2`, `UART3`, `UART4`. A legacy `COMx` alias is not assigned. Open the
published device-interface path with Win32 serial APIs, or use the supplied
tool (8N1, no flow control):

```powershell
.\Pi5UartTool.exe list
.\Pi5UartTool.exe send UART0 115200 0x48 0x69 0x0A
.\Pi5UartTool.exe read UART0 115200 32 2000
```

Each command owns the port only while it runs. `read` waits up to the specified
milliseconds; bytes sent while the port is closed are not retained. The driver
supports 300–1,000,000 baud when the divisor fits within 2%, 5–8 data bits,
parity and one/two stop bits. Hardware/software flow control and modem inputs
are not exposed by the two-pin routes. The receive software ring holds 16 KiB;
overrun is reported through the serial error state.

Hardware loopback on UART0 passed 9,600, 115,200 and 921,600 baud, 7E1 and 8N2.
All four ports passed GPIO arbitration, empty-read timeout, pending-read
cancellation and pin release. Other ports' physical TX/RX loopbacks have not
been tested. PIO TX cancellation can wait for at most the bounded hardware
FIFO to drain; no hardware flow control can stall it indefinitely.

Build: `.\build.ps1 -Driver rp1-uart -Configuration Debug -Analyze`.
