# SPDX-License-Identifier: BSD-2-Clause-Patent
<#
.SYNOPSIS
Verify and install the complete ARM64 release package in an elevated terminal.
.PARAMETER TrustTestCertificate
Trust this release's public certificate in LocalMachine Root and TrustedPublisher.
.PARAMETER VerifyOnly
Check hashes and signatures without staging or installing drivers.
#>
[CmdletBinding()]
param(
    [string]$PackageRoot = $PSScriptRoot,
    [switch]$TrustTestCertificate,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PackageRoot = (Resolve-Path -LiteralPath $PackageRoot).ProviderPath.TrimEnd('\')
$manifest = Get-Content (Join-Path $PackageRoot 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.SchemaVersion -ne 1 -or $manifest.Architecture -ne 'ARM64' -or $manifest.Configuration -ne 'Release') {
    throw 'This installer requires a complete ARM64 Release manifest.'
}
if (!$VerifyOnly -and $env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { throw 'Run native ARM64 PowerShell on the Raspberry Pi.' }
function Package-Path([string]$Relative) {
    $path = [IO.Path]::GetFullPath((Join-Path $PackageRoot $Relative))
    if (!$path.StartsWith($PackageRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Invalid package path: $Relative" }
    return $path
}
foreach ($file in $manifest.Files) {
    $path = Package-Path $file.Path
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.SHA256) {
        throw "Release file missing or changed: $($file.Path)"
    }
}
$certificatePath = Package-Path 'RPi5-test.cer'
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
if ($certificate.Thumbprint -ne $manifest.CertificateThumbprint) { throw 'Release certificate mismatch.' }
if ($TrustTestCertificate -or !$VerifyOnly) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this installer in an elevated PowerShell terminal.'
    }
}
if ($TrustTestCertificate) {
    foreach ($store in 'Cert:\LocalMachine\Root','Cert:\LocalMachine\TrustedPublisher') {
        Import-Certificate -FilePath $certificatePath -CertStoreLocation $store | Out-Null
    }
}
foreach ($file in $manifest.Files) {
    if ([IO.Path]::GetExtension($file.Path) -notin '.sys','.dll','.cat') { continue }
    $signature = Get-AuthenticodeSignature -LiteralPath (Package-Path $file.Path)
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $manifest.CertificateThumbprint) {
        throw "Untrusted signature: $($file.Path). Use -TrustTestCertificate to trust this release's certificate."
    }
}
foreach ($package in $manifest.Packages) {
    if ($package.Inf -notin $manifest.Files.Path) { throw "INF absent from manifest: $($package.Inf)" }
}
Write-Host "Verified $($manifest.Packages.Count) ARM64 Release packages."
if ($VerifyOnly) { return }

# DiInstallDriver handles both PnP packages and the primitive NVRAM driver.
# https://learn.microsoft.com/windows-hardware/drivers/develop/creating-a-primitive-driver
if (!('Pi5DriverInstall' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Pi5DriverInstall {
    [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DiInstallDriverW(IntPtr parent, string inf, uint flags,
        [MarshalAs(UnmanagedType.Bool)] out bool reboot);
}
'@
}
$reboot = $false
foreach ($package in $manifest.Packages) {
    & pnputil.exe /add-driver (Package-Path $package.Inf)
    if ($LASTEXITCODE -notin 0,3010) { throw "Staging failed: $($package.Name) (exit $LASTEXITCODE)" }
    if ($LASTEXITCODE -eq 3010) { $reboot = $true }
}
foreach ($package in $manifest.Packages) {
    $needsReboot = $false
    if (![Pi5DriverInstall]::DiInstallDriverW([IntPtr]::Zero, (Package-Path $package.Inf), 0, [ref]$needsReboot)) {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Installation failed: $($package.Name): $([ComponentModel.Win32Exception]::new($errorCode).Message) ($errorCode)"
    }
    if ($needsReboot) { $reboot = $true }
    Write-Host "Installed or staged $($package.Name)."
}
if ($reboot) { Write-Host 'Restart Windows to finish installing the drivers.' }
else { Write-Host 'Installation finished. Windows retains a newer or better-ranked installed driver.' }
