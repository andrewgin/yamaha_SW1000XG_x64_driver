# SW1000Common.ps1 - shared by Install-Driver.ps1, Run-HwTest.ps1 and
# Uninstall-Driver.ps1 (dot-sourced). Windows PowerShell 5.1 and 7.
#
# Locale-independent on purpose: no parsing of pnputil or bcdedit text, whose
# labels are translated. Test mode and Secure Boot come from the registry,
# driver packages from Get-WindowsDriver, results from exit codes.

$Sw = @{
    ServiceKey  = 'HKLM:\SYSTEM\CurrentControlSet\Services\ysw1000xg'
    Params      = 'HKLM:\SYSTEM\CurrentControlSet\Services\ysw1000xg\Parameters'
    HardwareId  = 'PCI\VEN_1073&DEV_1000*'
    InfName     = 'ysw1000xg.inf'
    CertSubject = 'CN=SW1000XG Test Signing (CI)'
    CertStores  = @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher')
}

function Assert-Administrator {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $admin = [System.Security.Principal.WindowsBuiltInRole]::Administrator
    if (-not (New-Object System.Security.Principal.WindowsPrincipal $id).IsInRole($admin)) {
        throw 'Run this from PowerShell opened with "Run as administrator".'
    }
}

# PowerShell unrolls a returned array, and a one-item result has no .Count
# under strict mode in Windows PowerShell 5.1: wrap list results at the call
# site, e.g. $devices = @(Get-Sw1000Device).

# Present SW1000XG cards (normally one).
function Get-Sw1000Device {
    return @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
             Where-Object { $_.InstanceId -like $Sw.HardwareId })
}

function Get-SwDeviceProperty([string] $InstanceId, [string] $Key) {
    $p = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $Key -ErrorAction SilentlyContinue
    if ($null -eq $p) { return $null }
    return $p.Data
}

# One value under the driver's Parameters key, or $null.
function Get-SwValue([string] $Name) {
    $item = Get-ItemProperty -Path $Sw.Params -ErrorAction SilentlyContinue
    if ($null -eq $item) { return $null }
    $prop = $item.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $null }
    return $prop.Value
}

function Set-SwDword([string] $Name, [int] $Value) {
    if (-not (Test-Path $Sw.Params)) { New-Item -Path $Sw.Params | Out-Null }
    New-ItemProperty -Path $Sw.Params -Name $Name -PropertyType DWord -Value $Value -Force | Out-Null
}

function Remove-SwValue([string] $Name) {
    if ($null -ne (Get-SwValue $Name)) { Remove-ItemProperty -Path $Sw.Params -Name $Name }
}

# Booted with test signing on? (bcdedit's output is localised; this is not.)
function Get-TestSigningState {
    $opts = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions -ErrorAction SilentlyContinue).SystemStartOptions
    return ($null -ne $opts) -and ($opts -match 'TESTSIGNING')
}

function Get-SecureBootState {
    $v = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -Name UEFISecureBootEnabled -ErrorAction SilentlyContinue
    if ($null -eq $v) { return 'unknown (legacy BIOS?)' }
    if ($v.UEFISecureBootEnabled -eq 1) { return 'on' }
    return 'off'
}

# Driver-store packages of this driver (oemNN.inf names).
function Get-SwDriverPackages {
    return @(Get-WindowsDriver -Online -ErrorAction SilentlyContinue |
             Where-Object { [System.IO.Path]::GetFileName($_.OriginalFileName) -ieq $Sw.InfName })
}

# Restart one card so the driver runs StartDevice again.
function Restart-Sw1000([string] $InstanceId) {
    & pnputil.exe /restart-device $InstanceId | Out-Null
    if ($LASTEXITCODE -eq 0) { return 'pnputil /restart-device' }
    Disable-PnpDevice -InstanceId $InstanceId -Confirm:$false
    Start-Sleep -Seconds 1
    Enable-PnpDevice -InstanceId $InstanceId -Confirm:$false
    return "disable/enable (pnputil exit $LASTEXITCODE)"
}

function Remove-SwTestCertificates([string] $KeepThumbprint = '') {
    foreach ($store in $Sw.CertStores) {
        Get-ChildItem $store | Where-Object { $_.Subject -eq $Sw.CertSubject -and $_.Thumbprint -ne $KeepThumbprint } |
            Remove-Item
    }
}
