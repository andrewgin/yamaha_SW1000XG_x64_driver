<#
.SYNOPSIS
    Removes the SW1000XG test driver from this PC.

.DESCRIPTION
    Run from an elevated PowerShell (Safe Mode works too). It:
      1. uninstalls every SW1000XG test driver package from the card and the
         driver store;
      2. deletes the driver's settings (microcode copy and test logs);
      3. removes the SW1000XG test certificates.

    Test mode stays on; TESTING.md says how to turn it off.

.PARAMETER KeepSettings
    Keep the microcode and test logs in the registry.

.PARAMETER KeepCertificate
    Keep the test certificates.
#>
param(
    [switch] $KeepSettings,
    [switch] $KeepCertificate
)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'SW1000Common.ps1')

Assert-Administrator

$packages = @(Get-SwDriverPackages)
foreach ($pkg in $packages) {
    Write-Host "Removing driver package $($pkg.Driver) ($($pkg.Version))..."
    & pnputil.exe /delete-driver $pkg.Driver /uninstall /force | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Warning "pnputil exit code $LASTEXITCODE for $($pkg.Driver)." }
}
if ($packages.Count -eq 0) { Write-Host 'No SW1000XG driver package installed.' }

if (-not $KeepSettings -and (Test-Path $Sw.Params)) {
    Remove-Item -Path $Sw.Params -Recurse
    Write-Host 'Removed the driver settings (microcode copy, test logs).'
}
if (-not $KeepCertificate) {
    Remove-SwTestCertificates
    Write-Host 'Removed the SW1000XG test certificates.'
}
Write-Host 'Done. The card now has no driver (it shows as an unknown multimedia device).'
