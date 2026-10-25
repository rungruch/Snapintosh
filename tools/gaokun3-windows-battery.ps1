# SPDX-License-Identifier: MIT
# Read-only battery sampling on the gaokun3 under Windows, for the Phase 1
# battery guard (docs/GAOKUN3-PHASE1-GUARDS.md#windows-battery-readings).
# Run it once per battery state, then copy each output file back for
# tools/replay-gaokun3-windows-battery.py.
#
#   powershell -ExecutionPolicy Bypass -File .\gaokun3-windows-battery.ps1 -State STATE
#              [-Samples N] [-Interval S] [-Output FILE]
#
#   STATE  ac-charging  AC adapter in, battery charging
#          ac-full      AC adapter in, battery full or held by the charge limit
#          battery      no adapter
#          weak-usb     a computer's USB port as the only source, under load
#
# Run it from an administrator PowerShell: the root\wmi battery classes
# refuse other users.
#
# What it reads, all with Get-CimInstance:
#   - root\wmi BatteryStatus: the AC, charging, discharging and critical
#     flags, voltage, charge and discharge rate, remaining capacity;
#   - root\wmi BatteryFullChargedCapacity, BatteryStaticData (designed
#     capacity, chemistry), BatteryCycleCount;
#   - Win32_Battery's charge percent; Win32_BIOS, Win32_ComputerSystem's
#     maker and model, and Win32_OperatingSystem's version.
# These are Windows' view through Huawei's battery driver, not the EC's
# bytes.
#
# What it never does:
#   - write anywhere but FILE, or call any CIM method;
#   - record serial numbers, unique IDs, device or instance paths, the
#     computer name, user names or network details.
#
# Stops (AGENTS.md rule 5): battery and weak-usb refuse to start below 55 %
# and stop at 50 %.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('ac-charging', 'ac-full', 'battery', 'weak-usb')]
    [string]$State,
    [ValidateRange(1, 2000)][int]$Samples = 24,
    [ValidateRange(3, 600)][int]$Interval = 5,
    [string]$Output = ''
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$StartMin = 55
$Floor = 50

function Fail([string]$why) {
    [Console]::Error.WriteLine("gaokun3-windows-battery: $why")
    exit 2
}

# One field: digits, letters and a few separators only, so a value can never
# split a record. ABSENT when the property is missing or null.
function Clean($value) {
    if ($null -eq $value) { return 'ABSENT' }
    $s = ([string]$value) -replace '[^A-Za-z0-9._,:/+=@-]', '_'
    if ($s.Length -eq 0) { return 'EMPTY' }
    if ($s.Length -gt 48) { $s = $s.Substring(0, 48) }
    return $s
}

function Flag($value) {
    if ($null -eq $value) { return 'ABSENT' }
    if ($value) { return '1' }
    return '0'
}

function Wmi([string]$class) {
    try { return @(Get-CimInstance -Namespace 'root\wmi' -ClassName $class -ErrorAction Stop) }
    catch { return @() }
}

function Cim([string]$class) {
    try { return @(Get-CimInstance -ClassName $class -ErrorAction Stop) }
    catch { return @() }
}

function Prop($objects, [string]$name) {
    if ($objects.Count -eq 0) { return $null }
    $p = $objects[0].PSObject.Properties[$name]
    if ($null -eq $p) { return $null }
    return $p.Value
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Fail 'run from an administrator PowerShell: root\wmi battery classes need it' }

$status = Wmi 'BatteryStatus'
if ($status.Count -ne 1) { Fail "expected one battery in root\wmi BatteryStatus, found $($status.Count)" }
$online = Prop $status 'PowerOnline'
$percent = Prop (Cim 'Win32_Battery') 'EstimatedChargeRemaining'
$wantOnline = $State -ne 'battery'
if ($null -eq $online) { Fail 'BatteryStatus has no PowerOnline flag' }
if ([bool]$online -ne $wantOnline) {
    Fail "-State $State needs the adapter online=$([int]$wantOnline), Windows reads $([int][bool]$online)"
}
if ($State -eq 'battery' -or $State -eq 'weak-usb') {
    if ($null -eq $percent) { Fail 'Win32_Battery has no charge percent' }
    if ([int]$percent -lt $StartMin) { Fail "battery at $percent %: charge to $StartMin % or more first" }
}

if ($Output -eq '') {
    $Output = Join-Path (Get-Location) ("gaokun3-windows-battery-$State-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')
}
if (Test-Path -LiteralPath $Output) { Fail "$Output exists; readings are never overwritten" }
$writer = New-Object System.IO.StreamWriter($Output, $false, (New-Object System.Text.UTF8Encoding($false)))
$writer.NewLine = "`n"
function Emit([string]$line) { $writer.WriteLine($line); $writer.Flush() }

$taken = 0
$end = 'INTERRUPTED'
try {
    Emit 'snapintosh-gaokun3-windows-battery format=1'
    Emit "run state=$State samples=$Samples interval=$Interval"
    $os = Cim 'Win32_OperatingSystem'
    Emit ("windows version=$(Clean (Prop $os 'Version')) build=$(Clean (Prop $os 'BuildNumber'))" +
          " arch=$(Clean (Prop $os 'OSArchitecture'))")
    $bios = Cim 'Win32_BIOS'
    $cs = Cim 'Win32_ComputerSystem'
    $date = Prop $bios 'ReleaseDate'
    if ($null -ne $date) { $date = ([datetime]$date).ToString('yyyy-MM-dd') }
    Emit ("dmi vendor=$(Clean (Prop $cs 'Manufacturer')) product=$(Clean (Prop $cs 'Model'))" +
          " bios_version=$(Clean (Prop $bios 'SMBIOSBIOSVersion')) bios_date=$(Clean $date)")
    $static = Wmi 'BatteryStaticData'
    Emit ("battery_design designed_mwh=$(Clean (Prop $static 'DesignedCapacity'))" +
          " full_mwh=$(Clean (Prop (Wmi 'BatteryFullChargedCapacity') 'FullChargedCapacity'))" +
          " cycles=$(Clean (Prop (Wmi 'BatteryCycleCount') 'CycleCount'))" +
          " chemistry=$(Clean (Prop $static 'Chemistry'))")
    $start = Get-Date
    for ($n = 1; $n -le $Samples; $n++) {
        Start-Sleep -Seconds $Interval
        $s = Wmi 'BatteryStatus'
        $percent = Prop (Cim 'Win32_Battery') 'EstimatedChargeRemaining'
        $elapsed = [int]((Get-Date) - $start).TotalSeconds
        Emit ("sample n=$n elapsed_s=$elapsed online=$(Flag (Prop $s 'PowerOnline'))" +
              " charging=$(Flag (Prop $s 'Charging')) discharging=$(Flag (Prop $s 'Discharging'))" +
              " critical=$(Flag (Prop $s 'Critical')) voltage_mv=$(Clean (Prop $s 'Voltage'))" +
              " remaining_mwh=$(Clean (Prop $s 'RemainingCapacity'))" +
              " charge_mw=$(Clean (Prop $s 'ChargeRate')) discharge_mw=$(Clean (Prop $s 'DischargeRate'))" +
              " percent=$(Clean $percent)")
        $taken = $n
        $onBattery = -not [bool](Prop $s 'PowerOnline')
        if ($State -eq 'battery' -or $State -eq 'weak-usb' -or $onBattery) {
            if ($null -eq $percent) { $end = 'STOPPED_UNREADABLE'; break }
            if ([int]$percent -le $Floor) {
                $end = 'STOPPED_FLOOR'
                [Console]::Error.WriteLine("battery at $percent %: connect the AC adapter now.")
                break
            }
        }
    }
    if ($end -eq 'INTERRUPTED' -and $taken -eq $Samples) { $end = 'COMPLETE' }
}
finally {
    Emit "end status=$end samples=$taken"
    $writer.Close()
    [Console]::Error.WriteLine("gaokun3-windows-battery: $end after $taken samples -> $Output")
}
