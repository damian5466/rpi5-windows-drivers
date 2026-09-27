# BCM2712 GPIO and pin control

`Pi5BcmGpio.sys` controls the main and always-on BCM2712 GPIO banks. It provides
pin configuration, GPIO I/O and main-bank interrupts, including the board power
button. The firmware selects the C0 or D0 pin layout. Always-on bank-zero data
access uses the firmware broker to coordinate with SD voltage switching.

It requires matching firmware exposing both `ACPI\RPI1020` controllers and
is a dependency of the board and Bluetooth drivers.
