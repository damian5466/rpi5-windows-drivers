# Raspberry Pi 5 Windows ARM64 drivers

Drivers for Raspberry Pi 5 with the matching [rpi5-uefi firmware](https://github.com/damian5466/rpi5-uefi).

Tested on **Windows 11 Pro 25H2 ARM64, build 26200.9539**.

| Driver | Documentation |
| --- | --- |
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
| CYW43455 Bluetooth H4 transport | [cyw-bluetooth](cyw-bluetooth/README.md) |
| Board supplies, LEDs and camera enables | [pi5-board](pi5-board/README.md) |
| Hardware graph and dependency lookup | [pi5-graph](pi5-graph/README.md) |
| VideoCore mailbox and RTC service | [pi5-mailbox](pi5-mailbox/README.md) |
| VideoCore firmware clock service | [pi5-fclk](pi5-fclk/README.md) |
| BCM2712 V3D reset service | [pi5-pm](pi5-pm/README.md) |
| BCM2712 multimedia IOMMU service | [pi5-iommu](pi5-iommu/README.md) |
| VideoCore VII hardware, copy and compute service | [pi5-v3d](pi5-v3d/README.md) |
| VideoCore VII Direct3D and HDMI graphics | [pi5-graphics](pi5-graphics/README.md) |
| RP1 PWM fan control | [rp1-fan](rp1-fan/README.md) |
| File-backed UEFI variable persistence | [pi5-nvram](pi5-nvram/README.md) |
| Stock NVMe compatibility patch | [nvme](nvme/README.md) |

## Building

Use x64 Windows with Visual Studio's ARM64 C++ tools, Windows SDK and WDK
**10.0.26100.0**, KMDF **1.33**, Inf2Cat, and Python 3. Supply a matching
`rpi5-uefi` checkout containing `NvramFileLib`; the NVRAM driver shares those
sources with the firmware. Run the following commands from this repository.

Build all 22 drivers supplied as source:

```powershell
.\build.ps1 -Driver source -FirmwareRoot C:\Sources\rpi5-uefi
```

Build just the graphics package, including the display driver, power filter,
and Direct3D DLL:

```powershell
.\build.ps1 -Driver pi5-graphics -Configuration Debug
```

`Release` is the default configuration. Packages are written to `Build/<driver>`;
`-Output` changes that location. Both configurations include PDBs. Graphics
Debug keeps optimization enabled with checked diagnostics; the other source
drivers disable optimization in Debug. `-Analyze` enables MSVC code analysis;
the imported graphics C++ code's static-analysis diagnostics are advisory.
Compiler warnings fail all driver builds under `/W4 /WX`.
`-WdkRoot`, `-SdkVersion`, `-WdkVersion`, `-KmdfKit`, `-KernelKit` and `-Inf2Cat`
can select alternative tool locations. Use `-Python C:\Python\python.exe` if
Python is not on PATH.

The default `-Driver all` also builds the NVMe patch. It requires original
ARM64 Windows `stornvme.sys` **10.0.26100.9539** and its matching INF:

| Input | SHA-256 |
| --- | --- |
| `stornvme.sys` | `10330d2cf54a03208c677713332c77e38ce5c0029d87cf7a0fd9af33e121d249` |
| `stornvme.inf` | `8cfb0401ac12f2ce0a9a2dfe80a3eaba999997cb4e7c293ab03be62ae9e1fed1` |

```powershell
.\build.ps1 -Driver all -FirmwareRoot C:\Sources\rpi5-uefi `
  -NvmeDriver C:\Inputs\stornvme.sys -NvmeInf C:\Inputs\stornvme.inf -NvmeAnyDevice
```

`-NvmeAnyDevice` uses the standard NVMe PCI class. Alternatively, pass
`-NvmeHardwareId 'PCI\VEN_1234&DEV_5678'` with the actual controller's ID.
Microsoft binaries are supplied by the builder and are not in this source repo.

## Signing and release ZIPs

`build.ps1` creates unsigned packages unless `-CertificateThumbprint` selects
a code-signing certificate in `Cert:\CurrentUser\My`. It then signs the runtime
binaries and catalogs and exports the public certificate as `RPi5-test.cer`.
Private keys remain in the certificate store.

For a local test-signing certificate, create and trust it in an elevated
PowerShell terminal on the build machine:

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=RPi5 driver test' `
  -CertStoreLocation Cert:\CurrentUser\My -HashAlgorithm SHA256
Export-Certificate -Cert $cert -FilePath .\RPi5-test.cer
Import-Certificate -FilePath .\RPi5-test.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath .\RPi5-test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
```

Build the complete set of 23 packages and its release archives:

```powershell
.\release.ps1 -FirmwareRoot C:\Sources\rpi5-uefi `
  -NvmeDriver C:\Inputs\stornvme.sys -NvmeInf C:\Inputs\stornvme.inf `
  -CertificateThumbprint $cert.Thumbprint -SourceRevision (git rev-parse HEAD)
```

`release.ps1` always builds Release in a fresh directory, uses the generic
NVMe binding, and verifies catalog membership and signatures before packaging.
It writes a driver ZIP, a separate symbols ZIP, and SHA-256 sidecars to
`Build/Releases`. Use `-Output` and `-Name` to change the destination and name.
The driver ZIP includes the public certificate, licenses, a file manifest and
`install.ps1`. Test-signed drivers require Windows test-signing mode; see
[`INSTALL.txt`](INSTALL.txt) for installation.
