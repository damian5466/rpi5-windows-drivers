# SPDX-License-Identifier: BSD-2-Clause-Patent
[CmdletBinding()]
param([string]$PackageRoot = (Join-Path $PSScriptRoot 'Build'))
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this installer in an elevated PowerShell session.'
}
$packages = [ordered]@{
    'rp1-service'='pi5rp1'; 'rp1-clocks'='pi5rp1clock'; 'rp1-gpio'='pi5gpio'
    'rp1-uart'='pi5uart'; 'rp1-i2c'='pi5i2c'; 'rp1-spi'='pi5spi'
    'rp1-ethernet'='pi5ethernet'; 'rp1-fan'='pi5fan'
}
$paths = @()
foreach ($entry in $packages.GetEnumerator()) {
    $directory = Join-Path $PackageRoot $entry.Key
    $inf = Join-Path $directory ($entry.Value + '.inf')
    if (!(Test-Path -LiteralPath $inf -PathType Leaf)) { throw "Missing $inf" }
    foreach ($extension in '.sys','.cat') {
        $file = Join-Path $directory ($entry.Value + $extension)
        if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing $file" }
        if ((Get-AuthenticodeSignature -LiteralPath $file).Status -ne 'Valid') {
            throw "A valid trusted signature is required: $file"
        }
    }
    $paths += $inf
}
$reboot = $false
foreach ($path in $paths) {
    & pnputil.exe /add-driver $path
    if ($LASTEXITCODE -notin 0,3010) { throw "Staging failed: $path" }
    if ($LASTEXITCODE -eq 3010) { $reboot = $true }
}
foreach ($path in $paths) {
    & pnputil.exe /add-driver $path /install
    # ERROR_NO_MORE_ITEMS also means the current driver is already preferred.
    # https://learn.microsoft.com/windows-hardware/drivers/devtest/pnputil-return-values
    if ($LASTEXITCODE -notin 0,259,3010) { throw "Installation failed: $path" }
    if ($LASTEXITCODE -eq 259) { Write-Host "Package staged without changing a device: $path" }
    if ($LASTEXITCODE -eq 3010) { $reboot = $true }
}
if ($reboot) { Write-Host 'Windows requested a reboot to finish installation.' }
else { Write-Host 'RP1 packages installed or staged. Check PnP status after any matching firmware update.' }
