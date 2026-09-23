# File-backed UEFI variable persistence driver

Experimental Windows ARM64 driver that persists runtime UEFI variable changes
for the matching rpi5-uefi firmware.

See the [repository README](../README.md) for shared build prerequisites and
signing instructions. Run the commands below from the repository root.

## Build

The driver needs the matching `rpi5-uefi` source checkout because it shares
`NvramFileLib.c` and `NvramFileLib.h` with the firmware. Pass its root directory
with `-FirmwareRoot`; no directory layout is assumed.

```powershell
.\build.ps1 -Driver pi5-nvram -FirmwareRoot C:\Sources\rpi5-uefi -Analyze
```

The package is written to `Build/pi5-nvram`, with `Pi5Nvram.sys` and
`pi5nvram.inf`. Builds using `-Driver source` or `-Driver all` also include this
driver and require `-FirmwareRoot`. That argument is unnecessary when building
Ethernet, platform, fan or NVMe individually.

The firmware must include the file-backed NVRAM changes; a firmware checkout
without `NvramFileLib` is rejected before compilation. The
[prepare-firmware.py](prepare-firmware.py) helper assembles firmware boot files
and variable stores as described below.

## Firmware with variable persistence

From this driver repository on Linux (or WSL), build the matching firmware and
assemble its boot files with fresh variable stores. Set `firmware_root` to the
separate firmware checkout:

```bash
firmware_root=/path/to/rpi5-uefi
(cd "$firmware_root" && ./build.sh --debug 0 --edk2-flags '-D FILE_NVRAM=TRUE')
python3 pi5-nvram/prepare-firmware.py "$firmware_root/Build/RPi5/RELEASE_GCC/FV/RPI_EFI.fd" "$firmware_root/config.txt" Build/boot/release
(cd "$firmware_root" && ./build.sh --debug 1 --edk2-flags '-D FILE_NVRAM=TRUE')
python3 pi5-nvram/prepare-firmware.py "$firmware_root/Build/RPi5/DEBUG_GCC/FV/RPI_EFI.fd" "$firmware_root/config.txt" Build/boot/debug
```

Copy the board DTB and D0 overlay described in the firmware repository's boot-file
instructions into each assembled boot folder. The assembly helper creates
`RPI_NV0.bin` and `RPI_NV1.bin` and adds their preload line to `config.txt`.
Install `Pi5Nvram` on Windows to persist runtime changes.

Preserve both existing variable files when updating firmware; do not overwrite
an installation's settings with a fresh pair. The helper preserves valid existing
pairs in its output directory. Each independent installation needs its own pair.
The firmware and variable files belong at the root of the FAT boot volume for
this configuration.
