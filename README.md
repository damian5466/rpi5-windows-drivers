# Raspberry Pi 5 Windows ARM64 drivers

Experimental drivers for the RP1 40-pin header, DMA, Ethernet and fan control,
BCM2712 GPIO, Bluetooth UART and temperature/RNG, board controls, a hardware
connection graph provider, and file-backed UEFI variable persistence, plus a
reproducible stock NVMe patch.
Use these drivers with the matching [rpi5-uefi firmware](https://github.com/damian5466/rpi5-uefi).

## Drivers

Each driver's README describes its purpose and dependencies.
Drivers are tested against Windows 11 build 26200.9539.

| Driver | Documentation |
| --- | --- |
| RP1 package architecture and header wiring | [RP1 header guide](RP1-HEADER.md) |
| RP1 interrupt service | [rp1-service](rp1-service/README.md) |
| RP1 clock service | [rp1-clocks](rp1-clocks/README.md) |
| RP1 GPIO / pin control | [rp1-gpio](rp1-gpio/README.md) |
| RP1 UART | [rp1-uart](rp1-uart/README.md) |
| RP1 I²C | [rp1-i2c](rp1-i2c/README.md) |
| RP1 SPI | [rp1-spi](rp1-spi/README.md) |
| RP1 DMA copy service | [rp1-dma](rp1-dma/README.md) |
| RP1 Ethernet | [rp1-ethernet](rp1-ethernet/README.md) |
| BCM2712 temperature and RNG | [bcm2712-platform](bcm2712-platform/README.md) |
| BCM2712 GPIO and power button | [bcm2712-gpio](bcm2712-gpio/README.md) |
| BCM2712 Bluetooth UART | [bcm2712-uart](bcm2712-uart/README.md) |
| CYW43455 Bluetooth H4 transport (bring-up) | [cyw-bluetooth](cyw-bluetooth/README.md) |
| Board supplies, LEDs and camera enables | [pi5-board](pi5-board/README.md) |
| Hardware graph and dependency lookup | [pi5-graph](pi5-graph/README.md) |
| VideoCore mailbox and RTC service | [pi5-mailbox](pi5-mailbox/README.md) |
| VideoCore firmware clock service | [pi5-fclk](pi5-fclk/README.md) |
| BCM2712 V3D reset service | [pi5-pm](pi5-pm/README.md) |
| BCM2712 multimedia IOMMU service | [pi5-iommu](pi5-iommu/README.md) |
| VideoCore VII hardware, copy and compute service | [pi5-v3d](pi5-v3d/README.md) |
| RP1 PWM fan control | [rp1-fan](rp1-fan/README.md) |
| File-backed UEFI variable persistence | [pi5-nvram](pi5-nvram/README.md) |
| Stock NVMe compatibility patch | [nvme](nvme/README.md) |

## Building

Build on x64 Windows with Visual Studio ARM64 C++ tools, Windows SDK/WDK
10.0.26100.0, KMDF 1.33, Inf2Cat, and Python 3. The build creates ARM64 SYS,
INF, CAT and PDB files for the C drivers in `Build/<driver>`. It uses `/W4 /WX`;
`-Analyze` also enables MSVC static analysis. Run commands from this repository's
root.

To build all drivers compiled from C sources:

```powershell
.\build.ps1 -Driver source -FirmwareRoot C:\Sources\rpi5-uefi -Analyze
```

Build the RP1 package, including its shared providers and header functions:

```powershell
.\build.ps1 -Driver rp1 -Configuration Debug -Analyze
```

`-Configuration Release` is the default. Debug uses `DBG=1` and disables
optimization; both configurations include separate PDBs. GPIO, UART, I²C and
SPI packages include desktop command-line tools. All hardware tests are opt-in;
installing a driver does not start a loopback or display test.

The firmware source argument is required by the included
[NVRAM driver](pi5-nvram/README.md#build). To build one package, use the
`-Driver` value shown in its README. The default is `-Driver all`, which also
includes the [NVMe patch and its required inputs](nvme/README.md).

`-WdkRoot`, `-SdkVersion`, `-WdkVersion` and `-Inf2Cat` select tool locations.
An extracted WDK can be selected with `-KernelKit` (headers in `include`, ARM64
libraries in `lib`) and `-KmdfKit` using the same layout. `-Output` selects the
output directory; the default is `Build` inside this repository.

## Signing

Packages are unsigned unless `-CertificateThumbprint` selects a code-signing
certificate in the current user's `My` store. Signing also exports the public
certificate as `RPi5-test.cer`; private keys stay in the certificate store.
Test-signed packages require a target configured for test signing.

## License

The original C drivers and build script carry the BSD-2-Clause-Patent SPDX identifier.
The adapted `cyw-bluetooth` transport is MS-PL; its embedded Cypress firmware
has a separate binary redistribution license. See that directory's README and
license files. Both licenses accompany its built package.
The NVMe patch recipe requires user-supplied Microsoft inputs; no stock or patched
Microsoft driver binaries, SDK/WDK files, or signing keys are distributed here.
