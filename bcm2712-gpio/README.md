# BCM2712 GPIO and pin control

`Pi5BcmGpio.sys` binds both `RPI1020` controllers. `_UID` selects main (0) or
always-on (1); `_HRV` selects the C0 or D0 pinctrl layout. Logical GPIO numbers
stay unchanged across revisions even where D0 packs mux/pull fields more
tightly. Missing pins and reserved pinctrl words are excluded.

Main GPIO supplies I/O, function configuration and edge/level interrupts.
The firmware's GPIO20 event reports the board power button to Windows;
Windows' selected power-button action determines its behavior. GpioClx
handles debounce. The driver only changes its GPIO aggregate bit in the
main L2 interrupt mask.

Always-on GPIO uses passive callbacks because bank-zero DATA access goes
through the firmware `_DSM` broker. It never accesses that DATA register
directly, or claims SD voltage-select GPIO3. This serializes board output
changes against the existing SD-voltage AML operation. AON interrupts are
not exposed.

Connections preserve unrelated mux/pull/register bits, reject incompatible
startup functions and restore released pins to their startup state. Existing
GPIO outputs stay driven during output ownership changes. The board driver
uses main GPIO28 and AON GPIO4/9; Bluetooth uses main GPIO24–29.

```powershell
.\build.ps1 -Driver bcm2712-gpio -Configuration Debug -Analyze
pnputil /add-driver Build\bcm2712-gpio\pi5bcmgpio.inf /install
```

A reboot may be required because the ACPI event client holds main GPIO20.
The one-second `BcmGpioDiagnostics` registry value under each controller's
`Device Parameters` records revision, status, ownership masks, IRQ counts and
only implemented pinctrl registers. It is diagnostic data, not a control API.

Hardware validation used a D0 Pi 5. C0 layouts and register isolation were
checked against the reference layout with host tests; C0 hardware and system
suspend/resume remain untested.
