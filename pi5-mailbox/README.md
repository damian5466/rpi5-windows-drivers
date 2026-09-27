# VideoCore mailbox and RTC service

`Pi5Mailbox.sys` owns the VideoCore firmware mailbox, serializes firmware
property requests, and supplies the ACPI Time and Alarm Device's real-time
clock operations from the Pi 5 PMIC. It provides firmware and clock queries,
clock control for kernel clients, and HDMI EDID and timing queries for the
display driver. Wake alarms are not enabled.

It requires matching firmware with the mailbox DMA aperture, ownership
contract and RTC operation region. Mailbox ownership lasts until reboot.
