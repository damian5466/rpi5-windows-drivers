# Raspberry Pi 5 Windows ARM64 drivers

Experimental drivers for RP1 Ethernet and fan control, BCM2712 temperature/RNG,
and file-backed UEFI variable persistence, plus a reproducible stock NVMe patch.
Use these drivers with the matching [rpi5-uefi firmware](https://github.com/damian5466/rpi5-uefi).

## Drivers

Each driver's README contains its build command and driver-specific instructions.
Drivers are tested against Windows 11 build 26100.9539.

| Driver | Documentation |
| --- | --- |
| RP1 Ethernet | [rp1-ethernet](rp1-ethernet/README.md) |
| BCM2712 temperature and RNG | [bcm2712-platform](bcm2712-platform/README.md) |
| RP1 PWM fan control | [rp1-fan](rp1-fan/README.md) |
| File-backed UEFI variable persistence | [pi5-nvram](pi5-nvram/README.md) |
| Stock NVMe compatibility patch | [nvme](nvme/README.md) |

## Building

Build on x64 Windows with Visual Studio ARM64 C++ tools, Windows SDK/WDK
10.0.26100.0, KMDF 1.33, Inf2Cat, and Python 3. The build creates ARM64 SYS,
INF, CAT and PDB files for the C drivers in `Build/<driver>`. It uses `/W4 /WX`;
`-Analyze` also enables MSVC static analysis. Run commands from this repository's
root.

To build all four drivers compiled from C sources:

```powershell
.\build.ps1 -Driver source -FirmwareRoot C:\Sources\rpi5-uefi -Analyze
```

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

The C drivers and build script carry the BSD-2-Clause-Patent SPDX identifier.
The NVMe patch recipe requires user-supplied Microsoft inputs; no stock or patched
Microsoft driver binaries, SDK/WDK files, or signing keys are distributed here.
