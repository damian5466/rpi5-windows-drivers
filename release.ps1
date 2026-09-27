# SPDX-License-Identifier: BSD-2-Clause-Patent
<#
.SYNOPSIS
Build every ARM64 driver in Release and create driver and symbol ZIPs.
.EXAMPLE
.\release.ps1 -FirmwareRoot C:\Sources\rpi5-uefi -NvmeDriver C:\Inputs\stornvme.sys -NvmeInf C:\Inputs\stornvme.inf -CertificateThumbprint <thumbprint>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$FirmwareRoot,
    [Parameter(Mandatory)][string]$NvmeDriver,
    [Parameter(Mandatory)][string]$NvmeInf,
    [Parameter(Mandatory)][ValidatePattern('^[0-9A-Fa-f]{40}$')][string]$CertificateThumbprint,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$Name = ('RPi5-Windows-ARM64-' + (Get-Date -Format yyyyMMdd)),
    [string]$Output = (Join-Path $PSScriptRoot 'Build\Releases'),
    [string]$SourceRevision = '',
    [string]$WdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10",
    [string]$SdkVersion = '10.0.26100.0',
    [string]$WdkVersion = '10.0.26100.0',
    [string]$KmdfVersion = '1.33',
    [string]$KernelKit = '',
    [string]$KmdfKit = '',
    [string]$Inf2Cat = '',
    [string]$Python = 'python',
    [switch]$Analyze
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Output = [IO.Path]::GetFullPath($Output)
New-Item $Output -ItemType Directory -Force | Out-Null
$archive = Join-Path $Output "$Name.zip"
$symbolArchive = Join-Path $Output "$Name-symbols.zip"
foreach ($path in @($archive, $symbolArchive, "$archive.sha256", "$symbolArchive.sha256")) {
    if (Test-Path -LiteralPath $path) { throw "Release output already exists: $path" }
}
$certificate = Get-Item "Cert:\CurrentUser\My\$CertificateThumbprint"
if (!$certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date)) {
    throw 'Select an unexpired code-signing certificate with a private key.'
}
$buildRoot = Join-Path $Output ('.work\release-' + [Guid]::NewGuid().ToString('N'))
$buildOutput = Join-Path $buildRoot 'build'
$packageRoot = Join-Path $buildRoot $Name
$symbolsRoot = Join-Path $buildRoot "$Name-symbols"
$buildArguments = @{
    Driver = 'all'; Configuration = 'Release'; Output = $buildOutput
    FirmwareRoot = $FirmwareRoot; NvmeDriver = $NvmeDriver; NvmeInf = $NvmeInf
    NvmeAnyDevice = $true; CertificateThumbprint = $CertificateThumbprint
    WdkRoot = $WdkRoot; SdkVersion = $SdkVersion; WdkVersion = $WdkVersion
    KmdfVersion = $KmdfVersion; KernelKit = $KernelKit; KmdfKit = $KmdfKit
    Inf2Cat = $Inf2Cat; Python = $Python; Analyze = $Analyze
}
& (Join-Path $PSScriptRoot 'build.ps1') @buildArguments

# Providers precede consumers during installation. All packages are staged first.
$names = @(
    'nvme','rp1-service','rp1-clocks','rp1-gpio','bcm2712-gpio','bcm2712-uart',
    'cyw-bluetooth','bcm2712-platform','rp1-fan','rp1-uart','rp1-i2c','rp1-spi',
    'rp1-dma','rp1-ethernet','pi5-board','pi5-graph','pi5-mailbox','pi5-fclk',
    'pi5-pm','pi5-iommu','pi5-v3d','pi5-graphics','pi5-nvram'
)
$built = @(Get-ChildItem $buildOutput -Directory | Where-Object Name -ne '.work' | Select-Object -ExpandProperty Name)
if (Compare-Object $names $built) { throw 'The complete release package set was not built.' }
New-Item $packageRoot,$symbolsRoot -ItemType Directory | Out-Null
$sign = Join-Path $WdkRoot "bin\$SdkVersion\x64\signtool.exe"
$packages = @()
foreach ($name in $names) {
    $source = Join-Path $buildOutput $name
    $destination = Join-Path $packageRoot $name
    New-Item $destination -ItemType Directory | Out-Null
    $infs = @(Get-ChildItem $source -Filter *.inf)
    $catalogs = @(Get-ChildItem $source -Filter *.cat)
    if ($infs.Count -ne 1 -or $catalogs.Count -ne 1) { throw "Invalid package: $name" }
    foreach ($file in Get-ChildItem $source -File | Where-Object Extension -in '.inf','.sys','.dll') {
        & $sign verify /pa /c $catalogs[0].FullName $file.FullName
        if ($LASTEXITCODE) { throw "Catalog verification failed: $file" }
    }
    foreach ($file in Get-ChildItem $source -File) {
        if ($file.Extension -eq '.pdb') {
            $symbolDirectory = Join-Path $symbolsRoot $name
            New-Item $symbolDirectory -ItemType Directory -Force | Out-Null
            Copy-Item $file.FullName $symbolDirectory
        } elseif ($file.Extension -in '.inf','.sys','.dll','.cat','.exe','.txt') {
            Copy-Item $file.FullName $destination
        }
    }
    Copy-Item (Join-Path $PSScriptRoot "$name\README.md") $destination
    $infText = Get-Content $infs[0].FullName -Raw
    if ($infText -notmatch '(?mi)^DriverVer\s*=\s*([^\r\n]+)') { throw "Missing DriverVer: $name" }
    $packages += [ordered]@{ Name = $name; Inf = "$name/$($infs[0].Name)"; DriverVer = $Matches[1].Trim() }
}
Copy-Item (Join-Path $buildOutput 'RPi5-test.cer') $packageRoot
Copy-Item "$PSScriptRoot\README.md","$PSScriptRoot\install.ps1","$PSScriptRoot\INSTALL.txt" $packageRoot
$files = @()
foreach ($file in Get-ChildItem $packageRoot -Recurse -File | Sort-Object FullName) {
    $files += [ordered]@{
        Path = $file.FullName.Substring($packageRoot.Length + 1).Replace('\','/')
        SHA256 = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifest = [ordered]@{
    SchemaVersion = 1; Architecture = 'ARM64'; Configuration = 'Release'
    SourceRevision = $SourceRevision; CertificateThumbprint = $CertificateThumbprint.ToUpperInvariant()
    CertificateExpires = $certificate.NotAfter.ToUniversalTime().ToString('o')
    Packages = $packages; Files = $files
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $packageRoot 'manifest.json') -Encoding UTF8
& (Join-Path $packageRoot 'install.ps1') -VerifyOnly
$symbolFiles = @()
foreach ($file in Get-ChildItem $symbolsRoot -Recurse -File | Sort-Object FullName) {
    $symbolFiles += [ordered]@{
        Path = $file.FullName.Substring($symbolsRoot.Length + 1).Replace('\','/')
        SHA256 = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
[ordered]@{ SchemaVersion = 1; SourceRevision = $SourceRevision; Files = $symbolFiles } |
    ConvertTo-Json -Depth 8 | Set-Content (Join-Path $symbolsRoot 'manifest.json') -Encoding UTF8

# Publish only completed ZIPs; build intermediates never enter either archive.
function Write-ReleaseArchive([string]$Root, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression,System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($Destination, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File | Sort-Object FullName) {
            # ZIP entry names use forward slashes on every platform. Windows
            # PowerShell's Compress-Archive can preserve native backslashes.
            $entryName = $file.FullName.Substring($Root.Length + 1).Replace('\','/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $file.FullName, $entryName, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $zip.Dispose()
    }
}
$temporaryArchive = Join-Path $buildRoot 'drivers.zip'
$temporarySymbols = Join-Path $buildRoot 'symbols.zip'
Write-ReleaseArchive -Root $packageRoot -Destination $temporaryArchive
Write-ReleaseArchive -Root $symbolsRoot -Destination $temporarySymbols
Move-Item $temporaryArchive $archive
Move-Item $temporarySymbols $symbolArchive
foreach ($path in @($archive,$symbolArchive)) {
    $hash = (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($path))" | Set-Content "$path.sha256" -Encoding ASCII
}
Write-Host "Release: $archive"
Write-Host "Symbols: $symbolArchive"
