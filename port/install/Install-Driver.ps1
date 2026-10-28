<#
.SYNOPSIS
    Installs this test build of the SW1000XG driver and its DSP microcode.

.DESCRIPTION
    Run from an elevated PowerShell in the unzipped test package
    (port/TESTING.md has the one-time machine setup):

      1. checks that Windows was started in test mode;
      2. checks your Yamaha driver download (the DSP microcode comes from it);
      3. trusts this build's test certificate (replacing older SW1000XG ones);
      4. removes any earlier SW1000XG test driver and installs this one;
      5. writes the microcode to the registry (Install-Microcode.ps1);
      6. restarts the card with the driver in its safe "identify only" mode.

    It does not run the hardware test; Run-HwTest.ps1 does that.

.PARAMETER YamahaSource
    Yamaha_SW10000XG_Win2000-XP.zip (or yswds.sys from it). If omitted, the
    script looks next to itself, one folder up, and in your Downloads folder.

.PARAMETER Force
    Continue even though test mode looks off (the driver will not load).
#>
param(
    [string] $YamahaSource,
    [switch] $Force
)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'SW1000Common.ps1')

Assert-Administrator

# 1. Test mode.
if (-not (Get-TestSigningState)) {
    $msg = 'Windows is not in test mode, so it will refuse to load this test-signed driver. ' +
           'Do the one-time setup in TESTING.md (Secure Boot off, "bcdedit /set testsigning on", reboot).'
    if (-not $Force) { throw $msg }
    Write-Warning $msg
}

$devices = @(Get-Sw1000Device)
if ($devices.Count -eq 0) {
    Write-Warning 'No SW1000XG found (PCI\VEN_1073&DEV_1000). The driver will be added to the driver store only.'
}

# 2. The Yamaha download: find it, then check it before changing anything.
if (-not $YamahaSource) {
    $downloads = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Downloads'
    $candidates = foreach ($dir in @($PSScriptRoot, (Split-Path $PSScriptRoot -Parent), $downloads)) {
        foreach ($name in @('Yamaha_SW10000XG_Win2000-XP.zip', 'yswds.sys')) { Join-Path $dir $name }
    }
    $YamahaSource = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $YamahaSource) {
        throw 'Could not find Yamaha_SW10000XG_Win2000-XP.zip. Put it in your Downloads folder, ' +
              'or pass its path: .\Install-Driver.ps1 -YamahaSource <path>'
    }
}
$microcodeScript = Join-Path $PSScriptRoot 'Install-Microcode.ps1'
$probe = Join-Path ([System.IO.Path]::GetTempPath()) ('sw1000-check-' + [guid]::NewGuid().ToString('N'))
try {
    & $microcodeScript -Source $YamahaSource -OutFile $probe | Out-Null
} finally {
    if (Test-Path $probe) { Remove-Item $probe }
}
Write-Host "Yamaha driver download OK."

# 3. Test certificate (CI makes a fresh one per build).
$cer = Join-Path $PSScriptRoot 'SW1000XG-Test.cer'
$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $cer
Remove-SwTestCertificates -KeepThumbprint $cert.Thumbprint
foreach ($store in $Sw.CertStores) {
    Import-Certificate -FilePath $cer -CertStoreLocation $store | Out-Null
}
Write-Host "Trusted test certificate $($cert.Thumbprint)."

# 4. Replace any earlier test driver.
foreach ($pkg in Get-SwDriverPackages) {
    Write-Host "Removing earlier driver package $($pkg.Driver) ($($pkg.Version))..."
    & pnputil.exe /delete-driver $pkg.Driver /uninstall /force | Out-Host
}
$inf = Join-Path $PSScriptRoot "driver\$($Sw.InfName)"
Write-Host 'Installing the driver...'
& pnputil.exe /add-driver $inf /install | Out-Host
$code = $LASTEXITCODE
# 0 = done, 3010 = done but reboot needed, 259 = staged, no device updated.
if (@(0, 3010, 259) -notcontains $code) {
    throw "pnputil could not install the driver (exit code $code). Copy this window's text into the GitHub issue."
}
if ($code -eq 3010) { Write-Warning 'Windows asks for a reboot to finish the install. Reboot, then run Run-HwTest.ps1.' }

if ($devices.Count -eq 0) {
    Write-Host 'Driver staged. Fit the card and rerun this script.'
    return
}
if (-not (Test-Path $Sw.ServiceKey)) {
    throw 'The driver service was not created, so the driver did not install on the card. ' +
          'Copy this window''s text into the GitHub issue.'
}

# 5. Microcode, then 6. a clean identify-only start of the new driver.
& $microcodeScript -Source $YamahaSource | Out-Host
Set-SwDword 'DiagMaxStage' 1
foreach ($d in $devices) {
    $how = Restart-Sw1000 $d.InstanceId
    Start-Sleep -Seconds 2
    $now = Get-PnpDevice -InstanceId $d.InstanceId
    Write-Host "$($now.FriendlyName): $($now.Status) (restarted via $how)"
}
if ($null -ne (Get-SwValue 'DiagCrashedStage')) {
    Write-Warning 'A crash from an earlier test is still recorded. Run-HwTest.ps1 will report it first.'
}
Write-Host ''
Write-Host 'Installed. Next: .\Run-HwTest.ps1' -ForegroundColor Green
