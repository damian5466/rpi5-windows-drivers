# RP1 PWM fan control

`Pi5Fan.sys` controls the RP1 cooling fan using a temperature-based PWM curve
and tachometer feedback. It consumes standard Windows WMI thermal readings.
An independent watchdog requests full speed when temperature data is missing
or stale, the fan stalls, or temperature reaches 75 degrees Celsius.

It requires matching firmware exposing `ACPI\RPI00F1`. The
[bcm2712-platform](../bcm2712-platform/README.md) driver supplies the Pi's
thermal readings; without a usable temperature provider the fan runs at full
speed.
