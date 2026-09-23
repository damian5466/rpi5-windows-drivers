# SPDX-License-Identifier: BSD-2-Clause-Patent
# Build the ARM64 kernel drivers from a local checkout with MSVC and the WDK.
[CmdletBinding()]
param(
    [ValidateSet('all', 'source', 'nvme', 'rp1-ethernet', 'bcm2712-platform', 'rp1-fan', 'pi5-nvram')]
    [string]$Driver = 'all',
    [string]$Output = (Join-Path $PSScriptRoot 'Build'),
    [string]$FirmwareRoot = '',
    [string]$WdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10",
    [string]$SdkVersion = '10.0.26100.0',
    [string]$WdkVersion = '10.0.26100.0',
    [string]$KmdfVersion = '1.33',
    [string]$KernelKit = '',
    [string]$KmdfKit = '',
    [string]$Inf2Cat = '',
    [string]$NvmeDriver = '',
    [string]$NvmeInf = '',
    [string]$NvmeHardwareId = '',
    [switch]$NvmeAnyDevice,
    [string]$Python = 'python',
    [string]$CertificateThumbprint = '',
    [switch]$Analyze
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Output = [IO.Path]::GetFullPath($Output)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) { throw 'Install Visual Studio C++ build tools, including ARM64 tools.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
if (!$vs) { throw 'Visual Studio ARM64 C++ tools were not found.' }
$init = 'call "' + $vs + '\VC\Auxiliary\Build\vcvarsall.bat" amd64_arm64 ' + $SdkVersion
$kernelInclude = Join-Path $WdkRoot "Include\$WdkVersion\km"
$kernelLib = Join-Path $WdkRoot "Lib\$WdkVersion\km\arm64"
if ($KernelKit) {
    $kernelInclude = Join-Path $KernelKit 'include'
    $kernelLib = Join-Path $KernelKit 'lib'
}
$kmdfInclude = Join-Path $WdkRoot "Include\wdf\kmdf\$KmdfVersion"
$kmdfLib = Join-Path $WdkRoot "Lib\wdf\kmdf\arm64\$KmdfVersion"
if ($KmdfKit) {
    $kmdfInclude = Join-Path $KmdfKit 'include'
    $kmdfLib = Join-Path $KmdfKit 'lib'
}
if (!$Inf2Cat) {
    foreach ($candidate in @("$WdkRoot\bin\$WdkVersion\x86\Inf2Cat.exe", "$WdkRoot\bin\x86\Inf2Cat.exe")) {
        if (Test-Path $candidate) { $Inf2Cat = $candidate; break }
    }
}
$sign = Join-Path $WdkRoot "bin\$SdkVersion\x64\signtool.exe"
function Require-File([string]$Path) {
    if (!$Path -or !(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing build prerequisite: $Path" }
}
Require-File (Join-Path $kernelInclude 'ntddk.h')
Require-File (Join-Path $kernelLib 'ntoskrnl.lib')
Require-File (Join-Path $kernelLib 'BufferOverflowFastFailK.lib')
Require-File $Inf2Cat
if ($CertificateThumbprint) {
    if ($CertificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$') { throw 'CertificateThumbprint must be a SHA-1 certificate thumbprint.' }
    Require-File $sign
}
$names = if ($Driver -in 'all','source') { @('rp1-ethernet','bcm2712-platform','rp1-fan','pi5-nvram') } else { @($Driver) }
if ($Driver -eq 'all') { $names = @('nvme') + $names }
if ('pi5-nvram' -in $names) {
    if (!$FirmwareRoot) { throw 'Supply -FirmwareRoot with the path to the matching rpi5-uefi checkout to build the NVRAM driver.' }
    $FirmwareRoot = [IO.Path]::GetFullPath($FirmwareRoot)
    $firmware = Join-Path $FirmwareRoot 'edk2-platforms\Platform\RaspberryPi'
    foreach ($relative in @('Library\NvramFileLib\NvramFileLib.c', 'Include\Library\NvramFileLib.h')) {
        if (!(Test-Path -LiteralPath (Join-Path $firmware $relative) -PathType Leaf)) {
            throw 'The NVRAM driver needs the matching rpi5-uefi sources. Supply -FirmwareRoot with the path to an rpi5-uefi checkout containing NvramFileLib.'
        }
    }
}
if ('nvme' -in $names) {
    if (!$NvmeDriver -or !$NvmeInf) { throw 'The NVMe build requires -NvmeDriver and -NvmeInf from matching original ARM64 Windows media. Use -Driver source to build only the four C-source drivers.' }
    if (!$NvmeAnyDevice -and !$NvmeHardwareId) { throw 'The NVMe build requires -NvmeHardwareId or -NvmeAnyDevice.' }
    Require-File $NvmeDriver
    Require-File $NvmeInf
    $NvmeDriver = (Resolve-Path -LiteralPath $NvmeDriver).ProviderPath
    $NvmeInf = (Resolve-Path -LiteralPath $NvmeInf).ProviderPath
}

foreach ($name in $names) {
    if ($name -eq 'nvme') {
        $work = Join-Path $Output ('.work\nvme-' + [Guid]::NewGuid().ToString('N'))
        [string[]]$nvmeBinding = if ($NvmeAnyDevice) { @('--any-device') } else { @('--hardware-id', $NvmeHardwareId) }
        & $Python (Join-Path $PSScriptRoot 'nvme\build.py') $NvmeDriver $NvmeInf $work @nvmeBinding
        if ($LASTEXITCODE) { throw 'NVMe package generation failed' }
        if ($CertificateThumbprint) {
            & $sign sign /fd SHA256 /s My /sha1 $CertificateThumbprint "$work\stornvme.sys"
            if ($LASTEXITCODE) { throw 'NVMe driver signing failed' }
        }
        & $Inf2Cat "/driver:$work" /os:10_GE_ARM64
        if ($LASTEXITCODE) { throw 'NVMe catalog generation failed' }
        if ($CertificateThumbprint) {
            & $sign sign /fd SHA256 /s My /sha1 $CertificateThumbprint "$work\pi5-stornvme.cat"
            if ($LASTEXITCODE) { throw 'NVMe catalog signing failed' }
        }
        $package = Join-Path $Output 'nvme'
        New-Item $package -ItemType Directory -Force | Out-Null
        Copy-Item "$work\stornvme.sys","$work\pi5-stornvme.inf","$work\pi5-stornvme.cat" $package -Force
        Write-Host "Built nvme -> $package"
        continue
    }
    $isNdis = $name -eq 'rp1-ethernet'
    if (!$isNdis) {
        if ($KmdfVersion -ne '1.33') { throw 'These INF packages require KMDF 1.33.' }
        Require-File (Join-Path $kmdfInclude 'wdf.h')
        Require-File (Join-Path $kmdfLib 'wdfdriverentry.lib')
        Require-File (Join-Path $kmdfLib 'wdfldr.lib')
    }
    $binary = switch ($name) {
        'rp1-ethernet' { 'Pi5Ethernet' }
        'bcm2712-platform' { 'Pi5Platform' }
        'rp1-fan' { 'Pi5Fan' }
        'pi5-nvram' { 'Pi5Nvram' }
    }
    $source = Join-Path $PSScriptRoot $name
    $package = Join-Path $Output $name
    # A separate work directory prevents stale objects and catalogs from entering a package.
    $work = Join-Path $Output ('.work\' + $name + '-' + [Guid]::NewGuid().ToString('N'))
    New-Item $work -ItemType Directory -Force | Out-Null
    Get-ChildItem $source -File | Where-Object { $_.Extension -in '.c','.h','.inf' } | Copy-Item -Destination $work
    if ($name -eq 'rp1-fan') {
        Copy-Item (Join-Path $PSScriptRoot 'bcm2712-platform\public.h') (Join-Path $work 'platform-public.h')
    }
    if ($name -eq 'pi5-nvram') {
        New-Item (Join-Path $work 'Library') -ItemType Directory | Out-Null
        Copy-Item "$firmware\Library\NvramFileLib\NvramFileLib.c" $work
        Copy-Item "$firmware\Include\Library\NvramFileLib.h" $work
        Copy-Item "$firmware\Include\Library\NvramFileLib.h" (Join-Path $work 'Library')
    }
    $sources = switch ($name) {
        'rp1-ethernet' { 'miniport.c gem.c' }
        'pi5-nvram' { 'driver.c NvramFileLib.c' }
        default { 'driver.c hardware.c' }
    }
    $objects = $sources.Replace('.c', '.obj')
    $defines = '/D_ARM64_ /D_ARM64_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00'
    $includes = '/I"' + $kernelInclude + '" /I"' + $work + '"'
    $libraries = '/LIBPATH:"' + $kernelLib + '" ntoskrnl.lib hal.lib BufferOverflowFastFailK.lib'
    $entry = 'GsDriverEntry'
    if ($isNdis) {
        Require-File (Join-Path $kernelInclude 'ndis.h')
        Require-File (Join-Path $kernelLib 'ndis.lib')
        $defines += ' /DNDIS_MINIPORT_DRIVER /DNDIS630_MINIPORT /DNDIS_WDM=1'
        $libraries = 'ndis.lib ' + $libraries
    } else {
        $defines += ' /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=33'
        $includes += ' /I"' + $kmdfInclude + '"'
        $libraries = '/LIBPATH:"' + $kmdfLib + '" wdfdriverentry.lib wdfldr.lib ' + $libraries
        $entry = 'FxDriverEntry'
    }
    $compile = 'cl /nologo /TC /W4 /WX /O2 /kernel /Zi /external:anglebrackets /external:W0 ' + $defines + ' ' + $includes + ' /c ' + $sources
    if ($Analyze) { $compile += ' /analyze /analyze:external-' }
    Push-Location $work
    try {
        & cmd.exe /d /s /c ($init + ' && ' + $compile)
        if ($LASTEXITCODE) { throw "$name compilation failed" }
        & cmd.exe /d /s /c ($init + ' && link /nologo /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:' + $entry + ' /MACHINE:ARM64 /DEBUG /INCREMENTAL:NO /OUT:' + $binary + '.sys ' + $objects + ' ' + $libraries)
        if ($LASTEXITCODE) { throw "$name linking failed" }
        $staging = Join-Path $work 'package'
        New-Item $staging -ItemType Directory | Out-Null
        Copy-Item "$binary.sys", ($binary.ToLowerInvariant() + '.inf') $staging
        if ($CertificateThumbprint) {
            & $sign sign /fd SHA256 /s My /sha1 $CertificateThumbprint "$staging\$binary.sys"
            if ($LASTEXITCODE) { throw "$name driver signing failed" }
        }
        & $Inf2Cat "/driver:$staging" /os:10_GE_ARM64
        if ($LASTEXITCODE) { throw "$name catalog generation failed" }
        if ($CertificateThumbprint) {
            & $sign sign /fd SHA256 /s My /sha1 $CertificateThumbprint "$staging\$($binary.ToLowerInvariant()).cat"
            if ($LASTEXITCODE) { throw "$name catalog signing failed" }
        }
        New-Item $package -ItemType Directory -Force | Out-Null
        Copy-Item "$staging\*" $package -Force
        Copy-Item "$binary.pdb" $package -Force
        Write-Host "Built $name -> $package"
    } finally {
        Pop-Location
    }
}

if ($CertificateThumbprint) {
    Export-Certificate -Cert "Cert:\CurrentUser\My\$CertificateThumbprint" -FilePath (Join-Path $Output 'RPi5-test.cer') | Out-Null
}
