# Raspberry Pi VideoCore mailbox and RTC service

`Pi5Mailbox` owns the BCM2712 firmware mailbox and serializes property requests
through a driver-owned DMA buffer. It supplies the ACPI Time and Alarm Device's
real-time clock operations from the Pi 5 PMIC and provides read-only firmware,
clock and RTC queries. An exclusive kernel clock controller can also set the
V3D clock rate and state through typed requests. Wake alarms are not enabled.
The mailbox driver does not control power domains, resets or display state.

The driver requires matching rpi5-uefi firmware with the mailbox DMA aperture,
ownership contract and RTC operation region. Ownership lasts until reboot;
driver replacement requires a reboot. No other Windows driver may access the
mailbox registers or share its DMA buffer.
