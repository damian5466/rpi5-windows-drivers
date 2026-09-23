# NVMe compatibility patch

Reproducible patch recipe for the stock Windows ARM64 NVMe driver.

See the [repository README](../README.md) for shared build prerequisites and
signing instructions. Run the commands below from the repository root.

## Patch inputs

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

## Build all driver packages

To include this patch when building all five packages, first set
`$nvmeHardwareId` using the Linux procedure below, then run:

```powershell
.\build.ps1 -Driver all -FirmwareRoot C:\Sources\rpi5-uefi -NvmeDriver C:\Inputs\stornvme.sys -NvmeInf C:\Inputs\stornvme.inf -NvmeHardwareId $nvmeHardwareId -Analyze
```

The [NVRAM driver](../pi5-nvram/README.md) requires the matching firmware sources.
The output for this package is `Build/nvme`, containing `stornvme.sys`,
`pi5-stornvme.inf` and `pi5-stornvme.cat`.

## Build for any PCIe NVMe controller

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

## Find the NVMe PCI hardware ID from Linux on the Pi

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
   variable with the all-driver build command above. See the shared
   [signing instructions](../README.md#signing) for package signing requirements.

If no NVMe controller is listed, check that Linux has enumerated the SSD over
PCIe before building the package. An SSD connected through a USB enclosure does
not expose its native PCI hardware ID through this procedure.
