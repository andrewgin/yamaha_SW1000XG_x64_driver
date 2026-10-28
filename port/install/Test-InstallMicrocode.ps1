<#
.SYNOPSIS
    Tests Install-Microcode.ps1 without any Yamaha data.

.DESCRIPTION
    Builds a synthetic "yswds.sys" of random bytes, then runs a copy of
    Install-Microcode.ps1 whose source hash and table hashes are replaced by the
    synthetic file's. The copy uses the service name ysw1000xg-citest, so the
    registry tests never touch a real installation. Checks:

      - the bundle from a .sys and from a zip equals the expected concatenation;
      - a wrong source file, a failing table hash and a zip without yswds.sys
        are rejected;
      - with -Registry (Windows, elevated): install refuses without the service
        key, writes and reads back the value, -RestartDevice copes with no card,
        and -Remove deletes the value.

    CI runs it under Windows PowerShell 5.1 and PowerShell 7
    (.github/workflows/microcode.yml).
#>
param([switch] $Registry)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'

$installer = Join-Path $PSScriptRoot 'Install-Microcode.ps1'
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('sw1000-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

function Get-Sha256Hex([byte[]] $Bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $digest = $sha.ComputeHash($Bytes) } finally { $sha.Dispose() }
    return ([System.BitConverter]::ToString($digest) -replace '-', '').ToLowerInvariant()
}

function Assert-True([bool] $Condition, [string] $What) {
    if (-not $Condition) { throw "FAIL: $What" }
    Write-Host "ok - $What"
}

function Assert-Throws([scriptblock] $Block, [string] $Pattern, [string] $What) {
    try { & $Block | Out-Null } catch {
        if ($_.Exception.Message -match $Pattern) { Write-Host "ok - $What"; return }
        throw "FAIL: $What (unexpected error: $($_.Exception.Message))"
    }
    throw "FAIL: $What (no error)"
}

function New-Zip([string] $Path, [string] $EntryName, [byte[]] $Bytes) {
    $zip = [System.IO.Compression.ZipFile]::Open($Path, 'Create')
    try {
        $s = $zip.CreateEntry($EntryName).Open()
        try { $s.Write($Bytes, 0, $Bytes.Length) } finally { $s.Dispose() }
    } finally { $zip.Dispose() }
}

try {
    # Synthetic source, the size of the real yswds.sys.
    $data = New-Object byte[] 247576
    (New-Object System.Random 1000).NextBytes($data)
    $source = Join-Path $work 'yswds.sys'
    [System.IO.File]::WriteAllBytes($source, $data)

    # Rewrite the installer's hashes for the synthetic file; collect the expected bundle.
    $text = [System.IO.File]::ReadAllText($installer)
    $expected = New-Object System.IO.MemoryStream
    $rx = "@\('(\w+)',\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*'([0-9a-f]{64})'\)"
    $text = [regex]::Replace($text, $rx, {
        param($m)
        $offset = [int]$m.Groups[2].Value - 0x10000
        $length = [int]$m.Groups[3].Value
        $slice = New-Object byte[] $length
        [System.Array]::Copy($data, $offset, $slice, 0, $length)
        $expected.Write($slice, 0, $length)
        $m.Value.Replace($m.Groups[4].Value, (Get-Sha256Hex $slice))
    })
    $expectedHash = Get-Sha256Hex $expected.ToArray()
    Assert-True ($expected.Length -eq 0x2500) 'manifest covers 0x2500 bytes'

    $text = $text -replace "\`$SourceSha256 = '[0-9a-f]{64}'", ("`$SourceSha256 = '" + (Get-Sha256Hex $data) + "'")
    $text = $text -replace 'Services\\ysw1000xg''', 'Services\ysw1000xg-citest'''
    Assert-True ($text.Contains('ysw1000xg-citest')) 'test copy uses the ysw1000xg-citest service key'
    $copy = Join-Path $work 'Install-Microcode.ps1'
    [System.IO.File]::WriteAllText($copy, $text)

    # Bundle from the .sys and from a zip (entry in a subfolder, as a user's repack might be).
    $out = Join-Path $work 'sys.bundle'
    & $copy -Source $source -OutFile $out
    Assert-True ((Get-Sha256Hex ([System.IO.File]::ReadAllBytes($out))) -eq $expectedHash) 'bundle from yswds.sys'

    $zipPath = Join-Path $work 'driver.zip'
    New-Zip $zipPath 'Yamaha/YSWDS.SYS' $data
    $out = Join-Path $work 'zip.bundle'
    & $copy -Source $zipPath -OutFile $out
    Assert-True ((Get-Sha256Hex ([System.IO.File]::ReadAllBytes($out))) -eq $expectedHash) 'bundle from the zip'

    # Rejections.
    $wrong = Join-Path $work 'wrong.sys'
    $bad = [byte[]]$data.Clone(); $bad[0x2000] = $bad[0x2000] -bxor 0xff
    [System.IO.File]::WriteAllBytes($wrong, $bad)
    Assert-Throws { & $copy -Source $wrong -OutFile (Join-Path $work 'x') } 'not the expected yswds\.sys' 'wrong source file rejected'

    $badTable = Join-Path $work 'BadTable.ps1'
    [System.IO.File]::WriteAllText($badTable, ($text -replace "(@\('dspa1',[^']*')[0-9a-f]{64}'", ('${1}' + ('0' * 64) + "'")))
    Assert-Throws { & $badTable -Source $source -OutFile (Join-Path $work 'x') } 'dspa1 failed its SHA-256' 'failing table hash rejected'

    $emptyZip = Join-Path $work 'empty.zip'
    New-Zip $emptyZip 'readme.txt' ([byte[]](65, 66))
    Assert-Throws { & $copy -Source $emptyZip -OutFile (Join-Path $work 'x') } 'No yswds\.sys inside' 'zip without yswds.sys rejected'

    if ($Registry) {
        $svc = 'HKLM:\SYSTEM\CurrentControlSet\Services\ysw1000xg-citest'
        if (Test-Path $svc) { Remove-Item $svc -Recurse }
        try {
            Assert-Throws { & $copy -Source $source } 'service is not installed' 'install refuses without the service key'
            New-Item -Path $svc | Out-Null
            & $copy -Source $source -RestartDevice
            $value = [byte[]](Get-ItemProperty -Path "$svc\Parameters" -Name Microcode).Microcode
            Assert-True ((Get-Sha256Hex $value) -eq $expectedHash) 'registry value holds the bundle'
            Assert-True ((Get-Item "$svc\Parameters").GetValueKind('Microcode') -eq 'Binary') 'registry value is REG_BINARY'
            & $copy -Remove
            Assert-True ($null -eq (Get-ItemProperty -Path "$svc\Parameters" -Name Microcode -ErrorAction SilentlyContinue)) '-Remove deletes the value'
        } finally {
            if (Test-Path $svc) { Remove-Item $svc -Recurse }
        }
    }
    Write-Host 'All installer tests passed.'
} finally {
    Remove-Item $work -Recurse -Force
}
