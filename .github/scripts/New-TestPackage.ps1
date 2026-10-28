<#
.SYNOPSIS
    CI: test-sign the Release driver and assemble the hardware test package.

.DESCRIPTION
    Makes a fresh self-signed code-signing certificate for this build (the
    private key never leaves the runner and dies with it), signs the driver
    package's .sys and .cat with it, and lays out:

      <OutDir>\<Label>\
        driver\        ysw1000xg.inf / .sys / .cat (test-signed)
        symbols\       ysw1000xg.pdb
        SW1000XG-Test.cer
        *.ps1          the install/test scripts from port/install
        TESTING.md     machine setup and the test routine
        release.txt    build identity (read into every report)

    The tester trusts the certificate with Install-Driver.ps1. Signing a PE
    file does not change its catalog hash, so the order of the two signatures
    does not matter.
#>
param(
    [Parameter(Mandatory = $true)] [string] $BinDir,      # port\bin\x64\Release
    [Parameter(Mandatory = $true)] [string] $OutDir,
    [Parameter(Mandatory = $true)] [string] $Label,
    [Parameter(Mandatory = $true)] [string] $BuildId
)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')

$src = Join-Path $BinDir 'ysw1000xg'                      # the WDK's package folder
$stage = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath((Join-Path $OutDir $Label))
foreach ($d in @('driver', 'symbols')) { New-Item -ItemType Directory -Force -Path (Join-Path $stage $d) | Out-Null }
foreach ($f in @('ysw1000xg.inf', 'ysw1000xg.sys', 'ysw1000xg.cat')) {
    Copy-Item (Join-Path $src $f) (Join-Path $stage 'driver')
}
Copy-Item (Join-Path $BinDir 'ysw1000xg.pdb') (Join-Path $stage 'symbols')

# Per-build test certificate (subject must match SW1000Common.ps1).
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=SW1000XG Test Signing (CI)' `
            -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 3072 `
            -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(3)
Export-Certificate -Cert $cert -FilePath (Join-Path $stage 'SW1000XG-Test.cer') | Out-Null

$signtool = Get-ChildItem -Path (Join-Path $repo 'port\packages'), "${env:ProgramFiles(x86)}\Windows Kits\10\bin" `
                -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName | Select-Object -Last 1
if ($null -eq $signtool) { throw 'signtool.exe not found' }
Write-Host "signtool: $($signtool.FullName)"
& $signtool.FullName sign /fd sha256 /sha1 $cert.Thumbprint /s My `
    (Join-Path $stage 'driver\ysw1000xg.sys') (Join-Path $stage 'driver\ysw1000xg.cat')
if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }
# The private key stays in the runner's store; the VM is discarded after the job.

Get-ChildItem (Join-Path $repo 'port\install') -Include *.ps1, *.psd1 -Recurse |
    Where-Object { $_.Name -notlike 'Test-*' } | Copy-Item -Destination $stage
Copy-Item (Join-Path $repo 'port\TESTING.md') $stage
@(
    "label $Label",
    "build $BuildId",
    "certificate $($cert.Thumbprint)",
    ('packaged {0:yyyy-MM-dd HH:mm} UTC' -f (Get-Date).ToUniversalTime())
) | Set-Content -Path (Join-Path $stage 'release.txt') -Encoding ascii

Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
