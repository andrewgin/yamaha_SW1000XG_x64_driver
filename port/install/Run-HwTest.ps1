<#
.SYNOPSIS
    Runs this build's hardware test on the SW1000XG and writes a report.

.DESCRIPTION
    Run from an elevated PowerShell in the unzipped test package, after
    Install-Driver.ps1. It sets how far the driver's bring-up may go
    (DiagMaxStage), restarts the card, waits for the driver's log, and writes
    a report to your Desktop. The report is also copied to the clipboard:
    paste it as a comment on the GitHub issue for this test.

    If the PC crashed (blue screen) during a test, just run this again after
    the reboot: the driver will have recorded the stage it died in and stays
    away from the card until this script has reported it.

    The report holds no personal data: no user or computer names.

.PARAMETER MaxStage
    Stop the bring-up after this stage (the issue says if you need it).
    Default: every stage this build has.

.PARAMETER CollectOnly
    Write the report without restarting the card.

.PARAMETER NoClipboard
    Do not copy the report to the clipboard.

.PARAMETER ReportPath
    Where to write the report (default: your Desktop).

.PARAMETER NoQuestions
    Skip this round's instructions and questions (round.psd1).
#>
param(
    [int] $MaxStage = 0,
    [switch] $CollectOnly,
    [switch] $NoClipboard,
    [string] $ReportPath,
    [switch] $NoQuestions
)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'SW1000Common.ps1')

Assert-Administrator

$lines = New-Object System.Collections.Generic.List[string]
function Add-Line([string] $Text = '') { $lines.Add($Text) }
function Add-Block([string] $Title, [string] $Body) {
    Add-Line "### $Title"
    Add-Line '```text'
    if ($Body) { Add-Line ($Body.TrimEnd()) } else { Add-Line '(none)' }
    Add-Line '```'
}

$devices = @(Get-Sw1000Device)
$crashed = Get-SwValue 'DiagCrashedStage'
$run = 'not run'

# This round's instructions and questions (round.psd1, written for each round).
$roundFile = Join-Path $PSScriptRoot 'round.psd1'
$round = $null
if (-not $NoQuestions -and (Test-Path $roundFile)) { $round = Import-PowerShellDataFile $roundFile }
$answers = New-Object System.Collections.Generic.List[string]

# ---- the test --------------------------------------------------------------
if ($CollectOnly) {
    $run = 'not run (-CollectOnly)'
} elseif ($devices.Count -eq 0) {
    $run = 'not run: no SW1000XG present'
} elseif (-not (Test-Path $Sw.ServiceKey)) {
    $run = 'not run: driver not installed (run Install-Driver.ps1)'
} elseif ($null -ne $crashed) {
    $run = "not run: the previous test crashed or hung in stage $crashed (reported below)"
} else {
    $stages = Get-SwValue 'DiagStageCount'
    $target = if ($MaxStage -gt 0) { $MaxStage } elseif ($null -ne $stages) { [int]$stages } else { 99 }
    $before = Get-SwValue 'DiagStartCount'
    if ($null -eq $before) { $before = 0 }

    if ($null -ne $round) {
        Write-Host ''
        foreach ($item in $round.Before) { Write-Host "* $item" -ForegroundColor Yellow }
        Read-Host 'Press Enter to start the test' | Out-Null
    }

    # A program holding the card's MIDI ports open makes Windows refuse to
    # restart it (round 3: MIDI-OX). Ask for MIDI-OX to be closed first.
    while (@(Get-Process -Name 'midiox' -ErrorAction SilentlyContinue).Count -gt 0) {
        Write-Warning 'MIDI-OX is running and would stop Windows restarting the card. Please close it.'
        Read-Host 'Press Enter once MIDI-OX is closed' | Out-Null
    }

    Set-SwDword 'DiagMaxStage' $target
    Set-SwDword 'DiagSelfTest' 1                    # the driver runs its self-tests once
    Write-Host "Restarting the card with stages 1-$target..."
    $restartedAt = Get-Date
    $how = Restart-Sw1000 $devices[0].InstanceId

    # A new start shows as a higher DiagStartCount and a log that begins with
    # that start number; it is finished when that log ends with END.
    $started = $false
    $done = $false
    $count = $before
    $deadline = (Get-Date).AddSeconds(20)
    while (-not $started -and (Get-Date) -lt $deadline) {
        $count = Get-SwValue 'DiagStartCount'
        $started = ($null -ne $count -and $count -gt $before)
        if (-not $started) { Start-Sleep -Milliseconds 500 }
    }
    if ($started) {
        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $deadline) {
            $log = Get-SwValue 'DiagLog'
            if ($null -ne $log -and $log -match "start $count;" -and $log.TrimEnd().EndsWith('END')) {
                $done = $true
                break
            }
            Start-Sleep -Milliseconds 500
        }
        $run = "stages 1-$target via $how; " + $(if ($done) { 'driver finished' } else { 'driver started but did NOT finish within 60 s' })
    } else {
        # Windows refused the restart. Name the program (Kernel-PnP event 225).
        Remove-SwValue 'DiagSelfTest'
        $blocker = 'unknown program'
        $veto = Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-PnP'; Id = 225; StartTime = $restartedAt.AddSeconds(-5) } -MaxEvents 1 -ErrorAction SilentlyContinue
        if ($null -ne $veto) {
            $blocker = ($veto.Message -replace '\s+', ' ')
            if ($veto.Message -match 'process id (\d+)') {
                $svc = @(Get-CimInstance Win32_Service -Filter "ProcessId=$($Matches[1])" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
                if ($svc.Count -gt 0) { $blocker += " [services in that process: $($svc -join ', ')]" }
            }
        }
        $run = "NOT run: Windows did not restart the card ($how). Blocked by: $blocker"
        Write-Warning 'Windows did not restart the card: a program is using it (details in the report). Close it and run .\Run-HwTest.ps1 again, or restart the PC.'
    }

    # This round's extra checks (scripts named in round.psd1); their output
    # goes into the report.
    $extras = New-Object System.Collections.Generic.List[string]
    if ($null -ne $round -and $done -and $null -ne $round['Extra']) {
        foreach ($name in $round['Extra']) {
            Write-Host ''
            Write-Host "Running $name..."
            try {
                $out = & (Join-Path $PSScriptRoot $name)
                $extras.Add("[$name]")
                foreach ($line in @($out)) { $extras.Add([string]$line) }
            } catch {
                $extras.Add("[$name] failed: $($_.Exception.Message)")
            }
        }
    }

    if ($null -ne $round -and $done) {
        Write-Host ''
        foreach ($q in $round.Questions) {
            $a = Read-Host $q
            $answers.Add("Q: $q")
            $answers.Add("A: $a")
        }
    }
}

# ---- the report -------------------------------------------------------------
$release = Join-Path $PSScriptRoot 'release.txt'
$os = Get-CimInstance Win32_OperatingSystem
$cs = Get-CimInstance Win32_ComputerSystem

Add-Line '## SW1000XG hardware test report'
Add-Line ('created {0:yyyy-MM-dd HH:mm} UTC; report format 1' -f (Get-Date).ToUniversalTime())
Add-Line ('build: ' + $(if (Test-Path $release) { (Get-Content $release) -join '; ' } else { 'unknown (no release.txt)' }))
Add-Line "Windows: $($os.Caption) $($os.Version) ($($os.OSArchitecture)); RAM $([math]::Round($cs.TotalPhysicalMemory / 1GB)) GB"
Add-Line "test mode: $(if (Get-TestSigningState) { 'on' } else { 'OFF' }); Secure Boot: $(Get-SecureBootState)"
Add-Line "test run: $run"
Add-Line

$cards = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like $Sw.HardwareId })
$cardText = foreach ($c in $cards) {
    $id = $c.InstanceId
    "$id"
    "  name: $($c.FriendlyName); present: $(Get-SwDeviceProperty $id 'DEVPKEY_Device_IsPresent'); status: $($c.Status); problem code: $(Get-SwDeviceProperty $id 'DEVPKEY_Device_ProblemCode')"
    "  driver: $(Get-SwDeviceProperty $id 'DEVPKEY_Device_DriverInfPath') $(Get-SwDeviceProperty $id 'DEVPKEY_Device_DriverVersion'); service: $(Get-SwDeviceProperty $id 'DEVPKEY_Device_Service')"
    "  location: $(Get-SwDeviceProperty $id 'DEVPKEY_Device_LocationInfo')"
}
if ((Test-Path variable:extras) -and $extras.Count -gt 0) {
    Add-Block 'Extra checks' ($extras -join "`r`n")
}
if ($answers.Count -gt 0) {
    Add-Block "Tester's answers (round $($round.Round))" ($answers -join "`r`n")
}
Add-Block 'Card' ($cardText -join "`r`n")

$microcode = Get-SwValue 'Microcode'
$values = foreach ($n in @('DiagMaxStage', 'DiagStageCount', 'DiagStartCount', 'DiagCrashedStage')) {
    "${n}: $(Get-SwValue $n)"
}
$values += 'Microcode: ' + $(if ($null -ne $microcode) { "$($microcode.Length) bytes" } else { 'missing' })
$values += 'driver packages: ' + ((Get-SwDriverPackages | ForEach-Object { "$($_.Driver) $($_.Version)" }) -join ', ')
Add-Block 'Driver settings' ($values -join "`r`n")

Add-Block 'Driver log (DiagLog)' (Get-SwValue 'DiagLog')
$crashLog = Get-SwValue 'DiagCrashLog'
if ($null -ne $crashed -or $null -ne $crashLog) {
    Add-Block "Log of the start that crashed or hung (stage $crashed)" $crashLog
}

$since = (Get-Date).AddDays(-14)
$bugchecks = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-WER-SystemErrorReporting'; StartTime = $since } -ErrorAction SilentlyContinue |
               ForEach-Object { '{0:yyyy-MM-dd HH:mm}  {1}' -f $_.TimeCreated, ($_.Message -replace '\s+', ' ') })
$dumps = @(Get-ChildItem "$env:SystemRoot\Minidump" -Filter *.dmp -ErrorAction SilentlyContinue |
           Where-Object { $_.LastWriteTime -gt $since } |
           ForEach-Object { '{0:yyyy-MM-dd HH:mm}  {1}  {2} KB' -f $_.LastWriteTime, $_.Name, [math]::Round($_.Length / 1KB) })
Add-Block 'Blue screens in the last 14 days' ((@($bugchecks) + @($dumps)) -join "`r`n")

$pnpProviders = @('Microsoft-Windows-Kernel-PnP', 'Microsoft-Windows-UserPnp', 'Service Control Manager')
$pnp = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = $pnpProviders; StartTime = (Get-Date).AddDays(-2) } -MaxEvents 500 -ErrorAction SilentlyContinue |
         Where-Object { $_.Message -match 'VEN_1073|ysw1000xg' } | Select-Object -First 15 |
         ForEach-Object { '{0:MM-dd HH:mm:ss}  {1} {2}: {3}' -f $_.TimeCreated, $_.ProviderName, $_.Id, ($_.Message -replace '\s+', ' ') })
Add-Block 'System events about the card (last 2 days, newest first)' ($pnp -join "`r`n")

# ---- tidy the crash marker so the next build can run -------------------------
if ($null -ne $crashed) {
    $safe = [math]::Max(1, [int]$crashed - 1)
    Set-SwDword 'DiagMaxStage' $safe
    Remove-SwValue 'DiagCrashedStage'
    Remove-SwValue 'DiagCrashLog'
    Add-Line "(crash marker cleared; DiagMaxStage set to $safe, the last stage known to be safe)"
}

# ---- save, scrub, hand over -------------------------------------------------
$text = $lines -join "`r`n"
foreach ($secret in @($env:COMPUTERNAME, $env:USERNAME) | Where-Object { $_ -and $_.Length -ge 3 }) {
    $text = $text -replace [regex]::Escape($secret), '<redacted>'
}
if (-not $ReportPath) {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $ReportPath = Join-Path $desktop ('SW1000XG-report-{0:yyyyMMdd-HHmmss}.txt' -f (Get-Date))
}
[System.IO.File]::WriteAllText($ReportPath, $text)

Write-Host ''
Write-Host $text
Write-Host ''
if (-not $NoClipboard) {
    Set-Clipboard -Value $text
    Write-Host 'The report is on your clipboard: paste it as a comment on the GitHub issue.' -ForegroundColor Green
}
Write-Host "Saved to $ReportPath"
