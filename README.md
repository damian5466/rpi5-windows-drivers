# Raspberry Pi 5 Windows ARM64 drivers

Experimental drivers for RP1 Ethernet and fan control, BCM2712 temperature/RNG,
and file-backed UEFI variable persistence, plus a reproducible stock NVMe patch.
Use these drivers with the matching [rpi5-uefi firmware](https://github.com/damian5466/rpi5-uefi).

Build on x64 Windows with Visual Studio ARM64 C++ tools, Windows SDK/WDK
10.0.26100.0, KMDF 1.33, Inf2Cat, and Python 3. The build creates ARM64 SYS,
INF, CAT and PDB files in `Build/<driver>`. It uses `/W4 /WX`; `-Analyze`
also enables MSVC static analysis. Run the commands from this repository's root.
Builds that include NVRAM require `-FirmwareRoot` to select the firmware sources.

```powershell
# Four drivers compiled from C sources:
.\build.ps1 -Driver source -FirmwareRoot C:\Sources\rpi5-uefi -Analyze

# All five packages, including the stock NVMe compatibility patch:
.\build.ps1 -FirmwareRoot C:\Sources\rpi5-uefi -NvmeDriver C:\Inputs\stornvme.sys -NvmeInf C:\Inputs\stornvme.inf -NvmeHardwareId $nvmeHardwareId -Analyze
```

`-WdkRoot`, `-SdkVersion`, `-WdkVersion` and `-Inf2Cat` select tool locations.
An extracted WDK can be selected with `-KernelKit` (headers in `include`, ARM64
libraries in `lib`) and `-KmdfKit` using the same layout. `-Output` selects the
output directory.

The NVRAM driver needs the matching `rpi5-uefi` source checkout because it shares
`NvramFileLib.c` and `NvramFileLib.h` with the firmware. Pass its root directory
with `-FirmwareRoot`, as in the examples above. No directory layout is assumed.

`-FirmwareRoot` is unnecessary when building Ethernet, platform, fan or NVMe
individually. The default output stays inside `Build`
directory. The firmware must include the file-backed
NVRAM changes; a firmware checkout without `NvramFileLib` is rejected before
compilation. The `pi5-nvram/prepare-firmware.py` helper assembles firmware boot
files and variable stores, as described below.

Packages are unsigned unless `-CertificateThumbprint` selects a code-signing
certificate in the current user's `My` store. Signing also exports the public
certificate as `RPi5-test.cer`; private keys stay in the certificate store.
Test-signed packages require a target configured for test signing.

### NVMe patch inputs

Only the patch recipe is included. Supply original ARM64 Windows
`stornvme.sys` version **10.0.26100.9539** and its matching `stornvme.inf`:

- SYS SHA-256: `10330d2cf54a03208c677713332c77e38ce5c0029d87cf7a0fd9af33e121d249`
- INF SHA-256: `8cfb0401ac12f2ce0a9a2dfe80a3eaba999997cb4e7c293ab03be62ae9e1fed1`

`nvme/build.py` verifies these hashes, switches DMA allocations
and matching frees to noncached memory, cleans PRP list cache lines before
publishing their addresses, removes conflicting import-call relocations, and
removes the invalidated signature. It checks the unsigned output hash before
writing a new package. Set `$nvmeHardwareId` to the target controller's PCI
hardware ID using the Linux procedure below, or use `-NvmeAnyDevice` as described
next. By default, the generated INF binds only to the supplied ID.
Other driver versions are rejected; controller compatibility
still requires hardware testing. The source files are never modified; Microsoft
binaries and SDK/WDK files are not included here.

#### Build for any PCIe NVMe controller

Pass `-NvmeAnyDevice` to match any standard PCIe NVMe controller using the PCI
class ID `PCI\CC_010802`, without looking up its vendor/device ID:

```powershell
.\build.ps1 -Driver nvme -NvmeDriver C:\Inputs\stornvme.sys -NvmeInf C:\Inputs\stornvme.inf -NvmeAnyDevice
```

This switch also works with the all-driver build. `-NvmeHardwareId` is optional
and ignored when `-NvmeAnyDevice` is set; without the switch, an ID is required.
For direct use of `nvme/build.py`, the equivalent option is `--any-device`
instead of `--hardware-id`. Only the INF matching changes; the binary patch,
required input versions, and signing requirements stay the same. Matching any
controller does not establish hardware compatibility with every SSD.

#### Find the NVMe PCI hardware ID from Linux on the Pi

Boot Linux on the Pi, for example Raspberry Pi OS from an SD card, with the
intended NVMe SSD connected through the Pi's PCIe connector/HAT. Windows does
not need to boot. Run these commands on the Pi that will use the SSD.

1. List its NVMe controllers:

   ```bash
   # Install only if lspci is missing (Raspberry Pi OS/Debian/Ubuntu):
   sudo apt install pciutils
   lspci -Dnn -d ::0108
   ```

   Example output with **illustrative IDs; use your own output**:

   ```text
   0000:01:00.0 Non-Volatile memory controller [0108]: Example NVMe controller [1234:5678] (rev 01)
   ```

   `0000:01:00.0` is the PCI address. The final `[1234:5678]` pair contains the
   vendor ID and device ID; `[0108]` is the device class. If several controllers
   appear, select the one corresponding to the SSD you will boot Windows from.

2. Use that controller's PCI address to print the exact Windows ID:

   ```bash
   pci_address=0000:01:00.0  # Replace with the address reported on your Pi.
   printf 'PCI\\VEN_%04X&DEV_%04X\n' \
     "$(cat "/sys/bus/pci/devices/$pci_address/vendor")" \
     "$(cat "/sys/bus/pci/devices/$pci_address/device")"
   ```

   For the illustrative values above, this prints `PCI\VEN_1234&DEV_5678`.
   It reads Linux's [PCI vendor and device attributes](https://docs.kernel.org/PCI/sysfs-pci.html)
   and formats them as a [Windows PCI identifier](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/identifiers-for-pci-devices).
   Keep all four hexadecimal digits in each value, including leading zeroes.
   The PCI address itself is not part of the Windows ID.

3. Copy the printed ID to PowerShell on the Windows **build host**, then build
   the NVMe package using the original driver and INF listed above:

   ```powershell
   # Replace the illustrative ID with the exact value printed on the Pi.
   $nvmeHardwareId = 'PCI\VEN_1234&DEV_5678'
   .\build.ps1 -Driver nvme -NvmeDriver C:\Inputs\stornvme.sys -NvmeInf C:\Inputs\stornvme.inf -NvmeHardwareId $nvmeHardwareId
   ```

   Keep the ID quoted so that `&` stays part of the value. You can also use this
   variable with the all-driver build command above. Signing requirements remain
   as described earlier.

If no NVMe controller is listed, check that Linux has enumerated the SSD over
PCIe before building the package. An SSD connected through a USB enclosure does
not expose its native PCI hardware ID through this procedure.

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

## License

The C drivers and build script carry the BSD-2-Clause-Patent SPDX identifier.
The NVMe patch recipe requires user-supplied Microsoft inputs; no stock or patched
Microsoft driver binaries, SDK/WDK files, or signing keys are distributed here.
