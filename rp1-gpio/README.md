# RP1 GPIO and pin control

`Pi5Gpio.sys` is a GpioClx controller for RP1 bank zero. The 40-pin header
exposes GPIO2–27; GPIO0/1 are reserved for HAT identification. Input, output,
pulls and edge/level interrupts are implemented. GpioClx arbitrates ownership,
emulates debounce/active-both, and handles function-config connections for the
UART, I²C and SPI controllers. Touched pins return to their boot configuration
when released. Board-internal banks are not mapped.

Use the desktop tool through the standard Windows GPIO API:

```powershell
.\Pi5GpioTool.exe list
.\Pi5GpioTool.exe read 22 up
.\Pi5GpioTool.exe watch 27 10
```

Run without arguments for the exact syntax. Only drive an output after checking
what is connected to that pin. The [header guide](../RP1-HEADER.md) lists the
peripheral pin conflicts and shared-provider installation requirements.

Build: `.\build.ps1 -Driver rp1-gpio -Configuration Debug -Analyze`.
Hardware bring-up verified input pulls, ownership, GPIO17→27 levels and exactly
25 rising / 25 falling interrupts, including after reboot.
