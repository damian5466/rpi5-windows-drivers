# RP1 SPI0

`Pi5Spi.sys` uses SpbCx with RP1's DesignWare APB SSI v4.02a. Firmware advertises
`RPI0060` only for header SPI0. Both chip selects are Windows-owned GPIO outputs,
held low across the whole request, including FIFO refill gaps and sequential
transfers. This avoids the controller's native chip-select deassertion when its
transmit FIFO empties. Other SPI instances and target mode are not exposed yet.

Supported: controller mode, four-wire single-bit SPI, 8-bit frames, modes 0–3,
100 kHz–4 MHz, read/write, sequential and full-duplex transfers. Unequal duplex
lengths are supported; additional transmitted bytes are zero. Hardware FIFO
in-flight counts bound RX occupancy. Requests are cancellable and have bounded
deadlines. There is no DMA or dual/quad mode.

```powershell
.\Pi5SpiTool.exe list
# Exchange three bytes with an attached device on SPI0 CS0, mode 0:
.\Pi5SpiTool.exe transfer SPI0 0 1000000 0 0x9F 0x00 0x00
```

SPI does not acknowledge device addresses, so a completed request alone does
not prove an external device is connected. Use a device-specific command and
validate its response. See [header wiring](../RP1-HEADER.md) and run the tool
without arguments for `read`/`write` syntax.

Physical MOSI/MISO loopback passed 4,096-byte transfers in every mode at
100 kHz, 1 MHz and 4 MHz. A separate GPIO input monitored CS0 and saw exactly
one falling/rising pair per request. Ten cancellations of active requests
released chip select and allowed successful close/reopen and subsequent
transfers. CS1 passed pin ownership and transfer completion checks; its
electrical chip-select waveform has not been measured.

Build: `.\build.ps1 -Driver rp1-spi -Configuration Debug -Analyze`.
