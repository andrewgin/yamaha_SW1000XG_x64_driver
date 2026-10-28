<#
.SYNOPSIS
    Plays notes through each SW1000XG MIDI output (the synth's ports and the
    MIDI OUT socket) using the Windows MIDI API (WinMM, the same interface
    MIDI-OX uses), and asks whether each was heard.

.DESCRIPTION
    Run by Run-HwTest.ps1 when round.psd1 lists it under Extra. Lists every
    MIDI output Windows knows (name, technology, voices, notes), then for each
    output whose name contains "SW1000": selects a different sound per port
    (piano, strings, guitar, organ), plays C-E-G-C, and asks the tester. The
    MIDI OUT socket can only be heard on a device connected to it. Returns
    the report lines.

.PARAMETER NoQuestions
    Play without asking (for a quick check).
#>
param([switch] $NoQuestions)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'

if (-not ('SwWinMM' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SwWinMM {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MIDIOUTCAPS {
        public ushort wMid;
        public ushort wPid;
        public uint vDriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szPname;
        public ushort wTechnology;
        public ushort wVoices;
        public ushort wNotes;
        public ushort wChannelMask;
        public uint dwSupport;
    }
    [DllImport("winmm.dll")] public static extern uint midiOutGetNumDevs();
    [DllImport("winmm.dll", EntryPoint = "midiOutGetDevCapsW", CharSet = CharSet.Unicode)]
    public static extern uint midiOutGetDevCaps(UIntPtr id, ref MIDIOUTCAPS caps, uint size);
    [DllImport("winmm.dll")] public static extern uint midiOutOpen(out IntPtr handle, uint id, IntPtr callback, IntPtr instance, uint flags);
    [DllImport("winmm.dll")] public static extern uint midiOutShortMsg(IntPtr handle, uint msg);
    [DllImport("winmm.dll")] public static extern uint midiOutReset(IntPtr handle);
    [DllImport("winmm.dll")] public static extern uint midiOutClose(IntPtr handle);

    public static MIDIOUTCAPS Caps(uint id, out uint result) {
        MIDIOUTCAPS caps = new MIDIOUTCAPS();
        result = midiOutGetDevCaps(new UIntPtr(id), ref caps, (uint)Marshal.SizeOf(typeof(MIDIOUTCAPS)));
        return caps;
    }
}
'@
}

$technology = @{ 1 = 'port'; 2 = 'synth'; 3 = 'square-wave synth'; 4 = 'FM synth'; 5 = 'mapper'; 6 = 'wavetable'; 7 = 'software synth' }
$lines = New-Object System.Collections.Generic.List[string]

function Send-Short([IntPtr] $Handle, [int] $Status, [int] $Data1, [int] $Data2) {
    $msg = [uint32]($Status -bor ($Data1 -shl 8) -bor ($Data2 -shl 16))
    [void][SwWinMM]::midiOutShortMsg($Handle, $msg)
}

# Windows may list new MIDI ports a moment after the driver registers them.
Start-Sleep -Seconds 2
$count = [SwWinMM]::midiOutGetNumDevs()
$ports = @()
for ($i = 0; $i -lt $count; $i++) {
    $result = [uint32]0
    $caps = [SwWinMM]::Caps([uint32]$i, [ref]$result)
    if ($result -ne 0) { $lines.Add("MIDI out ${i}: caps error $result"); continue }
    $tech = if ($technology.ContainsKey([int]$caps.wTechnology)) { $technology[[int]$caps.wTechnology] } else { "type $($caps.wTechnology)" }
    $lines.Add(("MIDI out {0}: '{1}' ({2}, voices {3}, notes {4}, channel mask 0x{5:x4})" -f
                $i, $caps.szPname, $tech, $caps.wVoices, $caps.wNotes, $caps.wChannelMask))
    if ($caps.szPname -like '*SW1000*') {
        $ports += , @($i, $caps.szPname)
    }
}
if ($ports.Count -eq 0) {
    $lines.Add('No SW1000 MIDI outputs found.')
    return $lines
}

$programs = @(0, 48, 24, 16)          # GM: acoustic piano, strings, nylon guitar, organ
$notes = @(60, 64, 67, 72)            # C4 E4 G4 C5
for ($p = 0; $p -lt $ports.Count; $p++) {
    $id, $name = $ports[$p]
    Write-Host ''
    Write-Host "Playing through '$name' now: listen to the card's output..." -ForegroundColor Yellow
    $handle = [IntPtr]::Zero
    $open = [SwWinMM]::midiOutOpen([ref]$handle, [uint32]$id, [IntPtr]::Zero, [IntPtr]::Zero, 0)
    if ($open -ne 0) {
        $lines.Add("'$name': midiOutOpen failed ($open)")
        continue
    }
    try {
        Send-Short $handle 0xC0 $programs[$p % 4] 0
        foreach ($n in $notes) {
            Send-Short $handle 0x90 $n 100
            Start-Sleep -Milliseconds $(if ($n -eq 72) { 900 } else { 350 })
            Send-Short $handle 0x80 $n 64
        }
        Start-Sleep -Milliseconds 300
        [void][SwWinMM]::midiOutReset($handle)
    } finally {
        [void][SwWinMM]::midiOutClose($handle)
    }
    $question = if ($name -like '*MIDI OUT*') {
        "Did a device connected to the card's MIDI OUT socket play four notes? (yes / no / nothing connected)"
    } else {
        "Did you hear four notes through '$name'? (yes / no / describe)"
    }
    $answer = if ($NoQuestions) { '(not asked)' } else { Read-Host $question }
    $lines.Add("'$name': played program $($programs[$p % 4]) C-E-G-C; heard: $answer")
}
return $lines
