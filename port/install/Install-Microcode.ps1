<#
.SYNOPSIS
    Installs the SW1000XG DSP microcode from your own copy of Yamaha's yswds.sys.

.DESCRIPTION
    The ysw1000xg driver needs about 9 KB of Yamaha DSP tables that cannot be
    redistributed, so the driver package ships
    none. This script takes them from the user's own Yamaha Win2000/XP driver:

      1. reads yswds.sys (or finds it inside Yamaha's original zip download);
      2. checks the file's SHA-256 against the one version this project knows;
      3. copies out the 34 tables listed below and checks each one's SHA-256;
      4. concatenates them in the driver's bundle order (SW_BLOB in
         port/inc/sw1000_blobs.h) and writes the result as the REG_BINARY value
         HKLM\SYSTEM\CurrentControlSet\Services\ysw1000xg\Parameters\Microcode.

    The driver checks the bundle's length and every table's SHA-256 again when
    it starts, and refuses to start without it (Code 10 in Device Manager).

    Install the driver first (that creates the service key), then run this
    script elevated with -RestartDevice. The manifest below must stay in step
    with tools/blobs_sw1000.csv and port/src/blobs.cpp; CI checks this
    (tools/check_manifest.py).

.PARAMETER Source
    Path to yswds.sys, or to the Yamaha zip that contains it
    (Yamaha_SW10000XG_Win2000-XP.zip).

.PARAMETER OutFile
    Write the bundle to this file instead of the registry. Needs no elevation.
    The file holds Yamaha's data: keep it for your own use only.

.PARAMETER RestartDevice
    After writing the registry value, restart every SW1000XG present so the
    driver loads the microcode now instead of at the next boot.

.PARAMETER Remove
    Delete the Microcode registry value.

.EXAMPLE
    .\Install-Microcode.ps1 -Source C:\Downloads\Yamaha_SW10000XG_Win2000-XP.zip -RestartDevice

.EXAMPLE
    .\Install-Microcode.ps1 -Source .\yswds.sys -OutFile .\microcode.bundle
#>
[CmdletBinding(DefaultParameterSetName = 'Install', SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true, Position = 0, ParameterSetName = 'Install')]
    [Parameter(Mandatory = $true, Position = 0, ParameterSetName = 'File')]
    [string] $Source,

    [Parameter(Mandatory = $true, ParameterSetName = 'File')]
    [string] $OutFile,

    [Parameter(ParameterSetName = 'Install')]
    [switch] $RestartDevice,

    [Parameter(Mandatory = $true, ParameterSetName = 'Remove')]
    [switch] $Remove
)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'

# yswds.sys v1.01.0014.1 (2002-01-30), the only version this project has analysed.
$SourceSha256 = '0e475f6af8a4e23af5d5fd3533533fc50433ae10e98aaab9d96368cda35eb4b4'
$ImageBase    = 0x10000     # file offset = VA - image base for every section
$ServiceKey   = 'HKLM:\SYSTEM\CurrentControlSet\Services\ysw1000xg'
$ValueName    = 'Microcode'
$HardwareId   = 'PCI\VEN_1073&DEV_1000*'

# name, VA, length (bytes), SHA-256 -- in bundle order (SW_BLOB).
$Manifest = @(
    @('bcr1MEL',             0x12dbc, 0x500, 'eacd0e4ae466af86b2316468a65b97b996ceb82e72e860e8af32444f3e9a2e73'),
    @('deqc1',               0x128bc, 0x500, 'a6ba15477df0bc1e048fcd98a20ec317b46a42e94dd53a1a817030a6d21fe155'),
    @('deqm1',               0x123bc, 0x500, '3a2ed94c344a45929bd2659f6d8255e10ee0a5c17eeae9a0ba918554303d84c5'),
    @('dspc1',               0x11c3c, 0x500, '16f0a76986df8cb603dfd49dc06e86b1b3e2a38ff81ec1100bdb08c5a7eb765e'),
    @('dspa1',               0x1213c, 0x280, '3ec51ca6eb8d806c7b75cb2f94f5d36666ef865636d562ab4a1cd78710b483f9'),
    @('dspm1_0',             0x1123c, 0x500, '631a277a63955afaba31bdf4c35278990fb7149391e933bf1b970213f29a3343'),
    @('dspm1_1',             0x1173c, 0x500, '0fcedfa8a1c106744df036557398eeb6d64d6d85aace837808a0207d676eef54'),
    @('n1mod0',              0x137bc, 0x100, 'a071c84600d87be9153a6b800050d86d2877f768b9dbb9823fef4902f233fd8f'),
    @('n1creg0CMEL',         0x13acc, 0x18,  '7143ac1dab4981acf06c3889b17d064691dacd229b4acfc64ebf39ce58565ac2'),
    @('n1fgtim0',            0x13a04, 0x80,  '98a4c64a9b450ca9079dfe72650227573009ee23d205d8421eed90d7a1aec89b'),
    @('n1fguram0',           0x13904, 0x100, '5341e6b2646979a70e57653007a1f310169421ec9bdd9f1a5648f75ade005af1'),
    @('n1creg0MEL',          0x13a84, 0x18,  'a9f43fa50b3192ccf184639cb47de95f6b4eb6800ce1aabb1fc3206f7fad3cf6'),
    @('n1greg0',             0x13ae4, 0x20,  '558a2825bee764e76bc1870ebf1dab535fb8b565df26802f8afc42c45e196ca6'),
    @('n1greg1',             0x15e60, 0x20,  '12b0ff10ef899ea80cac5cb747817fd6fa4200e9f6a9d381d96a4914d88339e9'),
    @('n1greg2',             0x181dc, 0x20,  '12b0ff10ef899ea80cac5cb747817fd6fa4200e9f6a9d381d96a4914d88339e9'),
    @('n1greg3',             0x1a558, 0x20,  '12b0ff10ef899ea80cac5cb747817fd6fa4200e9f6a9d381d96a4914d88339e9'),
    @('n1greg4',             0x1c8d4, 0x20,  '12b0ff10ef899ea80cac5cb747817fd6fa4200e9f6a9d381d96a4914d88339e9'),
    @('prerec_306f0',        0x306f0, 0x2c,  '85759b3811ff7dc47b03792ac85317be51431a3f9e01dcafce317ed736a391b0'),
    @('prerec_226a0',        0x226a0, 0x2c,  '4b27d441db7331b18a68a7bc3882b8203fafbb88751a7bb7c0044bed544a4f0e'),
    @('prerec_226cc',        0x226cc, 0x2c,  '3d37a5f445e7d0037f1cc653b76b15b7efe14b1a6ba7aa1f2ae4b1ac229bdda4'),
    @('prerec_226f8',        0x226f8, 0x2c,  '2971ee05c37908c1e9115ae10a636af807a8d7666d36cad5e6d8e21e8ba979d2'),
    @('prerec_22724',        0x22724, 0x10,  '3d0a787e15111027c66760ca16ee55036b5e5a13ff10ec0875d588e995e94667'),
    @('prerec_22734',        0x22734, 0x10,  'ffe3f061c5912afd2b7bc99f3879b78bfed14cdddb89534a86d42ffeb16d4083'),
    @('playmode_dspc_234d0', 0x234d0, 0x10,  '2666f1a90cde51c85bf501d41f0199709097f77f41e30aafed429fbbd76a2933'),
    @('playmode_dspc_234e0', 0x234e0, 0x10,  '40231e534bdaef895ee99d550626875339acdbd2b0be38bea389c865a2f87788'),
    @('recmode_dspc_234f0',  0x234f0, 0xc,   '9126cc43e40fdd9f0ac1aaca87dfa559cdd3a8f8fab39884356593890e2ef662'),
    @('recmode_dspc_234fc',  0x234fc, 0xc,   '58f6982fed1846182d4218ce27d734e9adad0abe236b7bd7dee9e031d878d283'),
    @('playmode_addr_23508', 0x23508, 0x8,   '9cc1f4c8cadb62105549134249d4b9d82fc63fbad4d844a7af78fb0b826901f9'),
    @('playmode_addr_23510', 0x23510, 0x8,   '3e673b26d432ec0fec20d7a9c61ad9d36e22df26e5eb7ae3d534e7b6489fae54'),
    @('playmode_addr_23518', 0x23518, 0x8,   'f4e10ade4808f99dac7318f14b54f44fa929e4cc733c09b11689a0e2adc1f5c1'),
    @('recmode_addr_23520',  0x23520, 0x4,   '9af0754e59d25340c505ae76b3db7d852d791ffbc5d8d574365aa00dbbc394b5'),
    @('recmode_addr_23524',  0x23524, 0x4,   '6c984eb4e413b3807efeebb5fd2ed29184557bd1aa9d5651302cb5dec67b0635'),
    @('recmode_addr_23528',  0x23528, 0x4,   '6eb945bba3b5cd96e9a2f5b37defc373aee0756593c5fde0b22f07ebc90633d3'),
    @('recmode_addr_2352c',  0x2352c, 0x4,   '91d0acdd9440ee2e69531a266ca95ba8d415ff4f5a6f5a72c097df6cafcec89a')
)

function Get-Sha256Hex([byte[]] $Bytes, [int] $Offset, [int] $Count) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $digest = $sha.ComputeHash($Bytes, $Offset, $Count) } finally { $sha.Dispose() }
    return ([System.BitConverter]::ToString($digest) -replace '-', '').ToLowerInvariant()
}

# Returns yswds.sys as one byte[] (the leading comma stops PowerShell unrolling it).
function Read-YswdsBytes([string] $Path) {
    $full = (Resolve-Path -LiteralPath $Path).ProviderPath
    if ([System.IO.Path]::GetExtension($full) -ine '.zip') {
        return , [System.IO.File]::ReadAllBytes($full)
    }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($full)
    try {
        $entry = $zip.Entries | Where-Object { $_.Name -ieq 'yswds.sys' } | Select-Object -First 1
        if ($null -eq $entry) { throw "No yswds.sys inside $full." }
        $in = $entry.Open()
        $out = New-Object System.IO.MemoryStream
        try { $in.CopyTo($out) } finally { $in.Dispose() }
        return , $out.ToArray()
    } finally {
        $zip.Dispose()
    }
}

function Build-Bundle([byte[]] $Data) {
    $hash = Get-Sha256Hex $Data 0 $Data.Length
    if ($hash -ne $SourceSha256) {
        throw "Source is not the expected yswds.sys v1.01.0014.1 (SHA-256 $hash). " +
              "Use the file from Yamaha's Win2000/XP SW1000XG driver download."
    }
    $bundle = New-Object System.IO.MemoryStream
    foreach ($m in $Manifest) {
        $name, $va, $length, $expected = $m
        $offset = $va - $ImageBase
        if ($offset + $length -gt $Data.Length) { throw "Table $name lies outside the file." }
        $got = Get-Sha256Hex $Data $offset $length
        if ($got -ne $expected) { throw "Table $name failed its SHA-256 check ($got)." }
        $bundle.Write($Data, $offset, $length)
    }
    return , $bundle.ToArray()
}

function Assert-Administrator {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $admin = [System.Security.Principal.WindowsBuiltInRole]::Administrator
    if (-not (New-Object System.Security.Principal.WindowsPrincipal $id).IsInRole($admin)) {
        throw 'Writing the registry needs an elevated PowerShell (Run as administrator), or use -OutFile.'
    }
}

function Restart-Sw1000Devices {
    $devices = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                 Where-Object { $_.InstanceId -like $HardwareId })
    if ($devices.Count -eq 0) {
        Write-Host 'No SW1000XG is present; the driver picks up the microcode when it next starts.'
        return
    }
    foreach ($d in $devices) {
        if (-not $PSCmdlet.ShouldProcess($d.InstanceId, 'Restart device')) { continue }
        & pnputil.exe /restart-device $d.InstanceId | Out-Host
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "pnputil could not restart $($d.InstanceId) (exit $LASTEXITCODE); disable and re-enable it in Device Manager, or reboot."
            continue
        }
        $now = Get-PnpDevice -InstanceId $d.InstanceId
        Write-Host "$($now.FriendlyName): $($now.Status)"
    }
}

$params = "$ServiceKey\Parameters"   # not Join-Path: it needs the HKLM: drive to exist

if ($Remove) {
    Assert-Administrator
    if ((Test-Path $params) -and $null -ne (Get-ItemProperty -Path $params -Name $ValueName -ErrorAction SilentlyContinue)) {
        if ($PSCmdlet.ShouldProcess("$params\$ValueName", 'Remove registry value')) {
            Remove-ItemProperty -Path $params -Name $ValueName
            Write-Host "Removed $params\$ValueName."
        }
    } else {
        Write-Host 'No microcode value is installed.'
    }
    return
}

$bundle = Build-Bundle (Read-YswdsBytes $Source)
$bundleHash = Get-Sha256Hex $bundle 0 $bundle.Length
Write-Host ("Verified yswds.sys and {0} tables: bundle 0x{1:x} bytes, SHA-256 {2}" -f
            $Manifest.Count, $bundle.Length, $bundleHash)

if ($PSCmdlet.ParameterSetName -eq 'File') {
    if ($PSCmdlet.ShouldProcess($OutFile, 'Write microcode bundle')) {
        $target = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutFile)
        [System.IO.File]::WriteAllBytes($target, $bundle)
        Write-Host "Wrote $target (Yamaha data: for your own use only)."
    }
    return
}

Assert-Administrator
if (-not (Test-Path $ServiceKey)) {
    throw "The ysw1000xg driver service is not installed. Install the driver first " +
          "(pnputil /add-driver ysw1000xg.inf /install), then run this script again with -RestartDevice."
}
if ($PSCmdlet.ShouldProcess("$params\$ValueName", 'Write microcode (REG_BINARY)')) {
    if (-not (Test-Path $params)) { New-Item -Path $params | Out-Null }
    New-ItemProperty -Path $params -Name $ValueName -PropertyType Binary -Value $bundle -Force | Out-Null
    $readBack = [byte[]](Get-ItemProperty -Path $params -Name $ValueName).$ValueName
    if ((Get-Sha256Hex $readBack 0 $readBack.Length) -ne $bundleHash) {
        throw 'The registry value did not read back correctly.'
    }
    Write-Host "Wrote $params\$ValueName."
}

if ($RestartDevice) {
    Restart-Sw1000Devices
} else {
    Write-Host 'Restart the SW1000XG (or reboot) for the driver to load the microcode, or rerun with -RestartDevice.'
}
