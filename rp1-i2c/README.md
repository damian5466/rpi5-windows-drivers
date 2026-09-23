# RP1 I²C

`Pi5I2c.sys` uses SpbCx with the DesignWare APB I²C controller. The `RPI0050`
compatible ID selects header instances 0–3. Instances 4–6 remain internal.
I²C1 on physical pins 3/5 is the default. Pin ownership is managed by Windows.
See the [header guide](../RP1-HEADER.md) for all routes and the tested OLED wiring.

Supported: 7-bit addresses 0x08–0x77, 100/400 kHz, read/write and repeated-start
sequences. Interrupts wake a passive worker; queued read commands are capped
by receive-FIFO capacity. Requests have bounded deadlines and cancellable
completion. Bus reset or recovery by driving arbitrary GPIO pulses is not
implemented. No transfer is attempted automatically at boot.

```powershell
.\Pi5I2cTool.exe list
# SSD1306 display on, for the tested OLED at address 0x3C:
.\Pi5I2cTool.exe write I2C1 0x3C 100000 0x00 0xAF
# Generic register read: replace ADDRESS/REGISTER for your device.
.\Pi5I2cTool.exe write-read I2C1 ADDRESS 100000 2 REGISTER
```

The tool also supports `read` and prints returned bytes in hex. Run without
arguments for syntax. The OLED needs its initialization sequence before
`display on` alone has an effect. The workspace hardware test supplies an
SSD1306 128×64 initialization and framebuffer example.

All four instances passed empty-bus error recovery and GPIO arbitration at
both speeds. I²C1 drove the physical OLED, with a visually confirmed checkerboard
and repeated 16 KiB writes at both speeds. Positive reads from a responding
peripheral still need hardware validation. See the header guide for the
Windows NACK mapping and partial-transfer limitations.

Build: `.\build.ps1 -Driver rp1-i2c -Configuration Debug -Analyze`.
