<#
.SYNOPSIS
    Checks the card's MIDI IN socket through the Windows MIDI API (WinMM): the
    tester plays a few notes on a keyboard connected to MIDI IN.

.DESCRIPTION
    Run by Run-HwTest.ps1 when round.psd1 lists it under Extra. Lists every
    MIDI input Windows knows, then, if a keyboard or controller is connected
    to the card's MIDI IN, records what arrives on "SW1000 MIDI IN" for 10
    seconds while the tester plays. Returns the report lines.

.PARAMETER NoQuestions
    Only list the inputs (for a check without a tester).
#>
param([switch] $NoQuestions)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'

if (-not ('SwMidiIn' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Threading;
public static class SwMidiIn {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct MIDIINCAPS {
        public ushort wMid;
        public ushort wPid;
        public uint vDriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szPname;
        public uint dwSupport;
    }
    delegate void MidiInProc(IntPtr handle, uint msg, IntPtr instance, IntPtr param1, IntPtr param2);

    [DllImport("winmm.dll")] public static extern uint midiInGetNumDevs();
    [DllImport("winmm.dll", EntryPoint = "midiInGetDevCapsW", CharSet = CharSet.Unicode)]
    static extern uint midiInGetDevCaps(UIntPtr id, ref MIDIINCAPS caps, uint size);
    [DllImport("winmm.dll")] static extern uint midiInOpen(out IntPtr handle, uint id, MidiInProc callback, IntPtr instance, uint flags);
    [DllImport("winmm.dll")] static extern uint midiInStart(IntPtr handle);
    [DllImport("winmm.dll")] static extern uint midiInStop(IntPtr handle);
    [DllImport("winmm.dll")] static extern uint midiInReset(IntPtr handle);
    [DllImport("winmm.dll")] static extern uint midiInClose(IntPtr handle);

    const uint CALLBACK_FUNCTION = 0x30000, MIM_DATA = 0x3C3, MIM_ERROR = 0x3C5;
    static readonly MidiInProc s_proc = OnMessage;          // kept alive for WinMM
    static ConcurrentQueue<string> s_events = new ConcurrentQueue<string>();

    static void OnMessage(IntPtr handle, uint msg, IntPtr instance, IntPtr param1, IntPtr param2) {
        if (msg == MIM_DATA || msg == MIM_ERROR) {
            s_events.Enqueue(string.Format("{0} {1:x6} at {2} ms", msg == MIM_DATA ? "data" : "ERROR",
                                           (uint)param1.ToInt64() & 0xFFFFFF, param2.ToInt64()));
        }
    }

    public static string Name(uint id) {
        MIDIINCAPS caps = new MIDIINCAPS();
        uint r = midiInGetDevCaps(new UIntPtr(id), ref caps, (uint)Marshal.SizeOf(typeof(MIDIINCAPS)));
        return r == 0 ? caps.szPname : "(caps error " + r + ")";
    }

    // Open device id, record for ms milliseconds, return the messages.
    public static string[] Capture(uint id, int ms, out uint result) {
        s_events = new ConcurrentQueue<string>();
        IntPtr handle;
        result = midiInOpen(out handle, id, s_proc, IntPtr.Zero, CALLBACK_FUNCTION);
        if (result != 0) return new string[0];
        try {
            result = midiInStart(handle);
            if (result == 0) Thread.Sleep(ms);
            midiInStop(handle);
            midiInReset(handle);
        } finally {
            midiInClose(handle);
        }
        return s_events.ToArray();
    }
}
'@
}

$lines = New-Object System.Collections.Generic.List[string]
Start-Sleep -Seconds 1
$count = [SwMidiIn]::midiInGetNumDevs()
$port = $null
for ($i = 0; $i -lt $count; $i++) {
    $name = [SwMidiIn]::Name([uint32]$i)
    $lines.Add("MIDI in ${i}: '$name'")
    if ($name -like '*SW1000*') { $port = @($i, $name) }
}
if ($null -eq $port) {
    $lines.Add('No SW1000 MIDI input found.')
    return $lines
}
if ($NoQuestions) { return $lines }

$id, $name = $port
$have = Read-Host "Is a MIDI keyboard or controller connected to the card's MIDI IN socket? (yes / no)"
if ($have -notmatch '^\s*y') {
    $lines.Add("'$name': not tested (nothing connected to MIDI IN)")
    return $lines
}
Write-Host ''
Write-Host "Recording from '$name' for 10 seconds: play a few notes now..." -ForegroundColor Yellow
$result = [uint32]0
$events = @([SwMidiIn]::Capture([uint32]$id, 10000, [ref]$result))
if ($result -ne 0) {
    $lines.Add("'$name': open/start failed (WinMM error $result)")
    return $lines
}
$lines.Add("'$name': $($events.Count) messages in 10 s")
foreach ($e in ($events | Select-Object -First 12)) { $lines.Add("  $e") }
return $lines
