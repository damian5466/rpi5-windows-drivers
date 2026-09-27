# File-backed UEFI variable persistence

`Pi5Nvram.sys` persists runtime UEFI variable changes to the firmware's
redundant variable files on the boot volume.

It requires matching rpi5-uefi firmware with file-backed NVRAM and uses the
same `NvramFileLib` implementation as that firmware. Existing variable files
contain the installation's firmware settings.
