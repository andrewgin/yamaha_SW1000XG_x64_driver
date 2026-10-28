<#
.SYNOPSIS
    CI: smoke-test the assembled hardware test package on a runner (no card).

.DESCRIPTION
    Exercises what can run without the card or the Yamaha file:
      - the test certificate is trusted and the package installs into the
        driver store (pnputil checks the catalog signature and file hashes);
      - Run-HwTest.ps1 -CollectOnly writes a report;
      - Uninstall-Driver.ps1 removes the package and the certificates.
    Runs under Windows PowerShell 5.1, as the tester will.
#>
param([Parameter(Mandatory = $true)] [string] $PackageDir)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
. (Join-Path $PackageDir 'SW1000Common.ps1')

function Assert-True([bool] $Condition, [string] $What) {
    if (-not $Condition) { throw "FAIL: $What" }
    Write-Host "ok - $what"
}

foreach ($store in $Sw.CertStores) {
    Import-Certificate -FilePath (Join-Path $PackageDir 'SW1000XG-Test.cer') -CertStoreLocation $store | Out-Null
}
& pnputil.exe /add-driver (Join-Path $PackageDir 'driver\ysw1000xg.inf') | Out-Host
Assert-True ($LASTEXITCODE -eq 0) "driver package accepted into the driver store (pnputil exit $LASTEXITCODE)"
Assert-True (@(Get-SwDriverPackages).Count -eq 1) 'Get-SwDriverPackages finds it'

Assert-True (Test-Path (Join-Path $PackageDir 'round.psd1')) 'package carries round.psd1'
$roundData = Import-PowerShellDataFile (Join-Path $PackageDir 'round.psd1')
Assert-True ($roundData.Round -ge 1 -and $roundData.Questions.Count -ge 1) 'round.psd1 has a round number and questions'

$midi = @(& (Join-Path $PackageDir 'Invoke-MidiPortTest.ps1') -NoQuestions)
$midi | ForEach-Object { Write-Host "  $_" }
Assert-True (($midi -join "`n") -match 'No SW1000 MIDI outputs found') 'Invoke-MidiPortTest.ps1 runs (WinMM interop compiles) and finds no card here'
$midiIn = @(& (Join-Path $PackageDir 'Invoke-MidiInTest.ps1') -NoQuestions)
$midiIn | ForEach-Object { Write-Host "  $_" }
Assert-True (($midiIn -join "`n") -match 'No SW1000 MIDI input found') 'Invoke-MidiInTest.ps1 runs (WinMM callback interop compiles) and finds no card here'

$report = Join-Path ([System.IO.Path]::GetTempPath()) 'sw1000-report.txt'
& (Join-Path $PackageDir 'Run-HwTest.ps1') -CollectOnly -NoClipboard -ReportPath $report | Out-Null
$text = [System.IO.File]::ReadAllText($report)
Assert-True ($text.StartsWith('## SW1000XG hardware test report')) 'report written'
Assert-True ($text.Contains('test run: not run (-CollectOnly)')) 'report records the collect-only run'
Assert-True ($text -match 'build: label ') 'report carries release.txt'
Write-Host $text

& (Join-Path $PackageDir 'Uninstall-Driver.ps1') | Out-Host
Assert-True (@(Get-SwDriverPackages).Count -eq 0) 'Uninstall-Driver.ps1 removed the package'
$left = @(foreach ($store in $Sw.CertStores) { Get-ChildItem $store | Where-Object { $_.Subject -eq $Sw.CertSubject } })
Assert-True ($left.Count -eq 0) 'Uninstall-Driver.ps1 removed the certificates'
Write-Host 'Package smoke test passed.'
