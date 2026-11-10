#Requires -Version 5.1
<#
Exact-device review/backup and optional per-device WinUSB interface GUID only.
NEVER binds/replaces a driver, downloads a package, elevates, disables policy,
or sends Bluetooth/USB/HCI traffic. Default is read-only Inspect plus own backup.
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 4096)]
    [ValidatePattern('(?i)^USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}(?:&MI_[0-9A-F]{2})?\\[^\\\x00-\x1f]+$')]
    [string] $InstanceID,
    [ValidatePattern('(?i)^USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}(?:&REV_[0-9A-F]{4})?(?:&MI_[0-9A-F]{2})?$')]
    [string] $ExpectedHardwareID,
    [string] $ConfirmInstanceID,
    [ValidateSet('Inspect', 'ConfigureGuid', 'RollbackGuid')]
    [string] $Action = 'Inspect',
    [switch] $Apply,
    [string] $BackupDirectory = (Join-Path $PSScriptRoot 'binding-backups'),
    [string] $RestoreFrom
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$TargetInstance = $InstanceID
$AzkiInterfaceGuid = '{a4b85fea-0ae3-4be1-b238-9dca9ed299a8}'
$RegistryPath = 'SYSTEM\CurrentControlSet\Enum\' + $TargetInstance + '\Device Parameters'
$GuidValueName = 'DeviceInterfaceGUIDs'

if ($env:OS -ne 'Windows_NT' -or [IntPtr]::Size -ne 8) {
    throw 'Use 64-bit Windows PowerShell. No device or registry operation was attempted.'
}
if ($Apply -and $Action -eq 'Inspect') {
    throw '-Apply requires an explicit ConfigureGuid or RollbackGuid action; it never binds a driver.'
}
if ($Apply -and (-not $ExpectedHardwareID -or
    -not [string]::Equals($ConfirmInstanceID, $TargetInstance, [StringComparison]::OrdinalIgnoreCase))) {
    throw 'Apply requires the reviewed ExpectedHardwareID and an exact typed ConfirmInstanceID. No device, driver or registry operation was attempted.'
}
if ($RestoreFrom -and $Action -ne 'RollbackGuid') {
    throw '-RestoreFrom is valid only with RollbackGuid.'
}

function Assert-WithinScriptDirectory([string] $Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') + '\'
    if (-not $full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Backup and restore files must stay under this task script directory.'
    }
    # Refuse existing junctions/symlinks along the destination path.
    $item = $full
    while ($item -and $item.Length -ge $root.TrimEnd('\').Length) {
        if (Test-Path -LiteralPath $item) {
            $info = Get-Item -LiteralPath $item -Force
            if (($info.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw ('Refusing reparse point: ' + $item)
            }
        }
        $parent = [IO.Path]::GetDirectoryName($item)
        if ($parent -eq $item) { break }
        $item = $parent
    }
    return $full
}

function Write-NewJson([string] $Path, $Value) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try {
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
        try { $writer.WriteLine((ConvertTo-Json -InputObject $Value -Depth 20)) }
        finally { $writer.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Read-PnpProperty([string] $KeyName) {
    try {
        $items = @(Get-PnpDeviceProperty -InstanceId $TargetInstance -KeyName $KeyName -ErrorAction Stop)
        if ($items.Count -ne 1) { throw 'Expected exactly one property result.' }
        return [ordered] @{ Available = $true; KeyName = $KeyName; Type = [string] $items[0].Type; Value = $items[0].Data }
    }
    catch {
        return [ordered] @{ Available = $false; KeyName = $KeyName; Type = $null; Value = $null; Error = $_.Exception.Message }
    }
}

function Read-RegistryValue([string] $Name) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,
                                                    [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $base.OpenSubKey($RegistryPath, $false)
        if ($null -eq $key) { return [ordered] @{ Available = $true; Exists = $false; Kind = $null; Value = $null } }
        try {
            if ($key.GetValueNames() -notcontains $Name) {
                return [ordered] @{ Available = $true; Exists = $false; Kind = $null; Value = $null }
            }
            return [ordered] @{ Available = $true; Exists = $true; Kind = [string] $key.GetValueKind($Name)
                Value = $key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
        }
        finally { $key.Dispose() }
    }
    finally { $base.Dispose() }
}

function Same-RegistryValue($First, $Second) {
    if ([bool] $First.Exists -ne [bool] $Second.Exists) { return $false }
    if (-not $First.Exists) { return $true }
    if ([string] $First.Kind -cne [string] $Second.Kind) { return $false }
    return ((ConvertTo-Json -InputObject $First.Value -Compress -Depth 8) -ceq
            (ConvertTo-Json -InputObject $Second.Value -Compress -Depth 8))
}

$BackupDirectory = Assert-WithinScriptDirectory $BackupDirectory
[IO.Directory]::CreateDirectory($BackupDirectory) | Out-Null
$devices = @(Get-PnpDevice -InstanceId $TargetInstance -ErrorAction Stop)
if ($devices.Count -ne 1 -or -not [string]::Equals([string] $devices[0].InstanceId, $TargetInstance,
                                                [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The exact dedicated USB instance was not found. No VID/PID fallback is permitted.'
}
$device = $devices[0]
$properties = [ordered] @{}
foreach ($name in @('Service', 'DriverInfPath', 'DriverVersion', 'DriverProvider', 'DriverDate',
                    'ProblemCode', 'HardwareIds', 'CompatibleIds', 'ContainerId', 'Parent',
                    'ClassGuid', 'DriverInfSection', 'DriverNodeStrongName')) {
    $properties[$name] = Read-PnpProperty ('DEVPKEY_Device_' + $name)
}
$guids = Read-RegistryValue $GuidValueName
$singularGuid = Read-RegistryValue 'DeviceInterfaceGUID'
$manualSteps = @(
    'This script NEVER performs a driver binding. Inspect the exact current service/INF in this backup.',
    ('In Device Manager open Properties > Details > Device instance path and verify exactly ' + $TargetInstance),
    'Save the original service/INF/version/driver-store and problem code in this backup before making a change.',
    'Only after user/root approval: Update driver > Browse my computer > Let me pick > Show all devices > Microsoft > WinUSB Device.',
    'Use only the normal Windows compatible selection and signed inbox Microsoft driver. If unavailable or Windows rejects it, stop; do not force an incompatible INF.',
    'Do not choose Intel, another same-VID/PID dongle, a class-wide match, a third-party certificate or a signing/SecureBoot bypass.',
    'After normal interactive binding, rerun Inspect and verify this exact instance has Service=WinUSB, DriverInfPath=winusb.inf, Provider=Microsoft and a healthy PnP status.',
    'Only after a separate reviewed Apply may this script append its dedicated interface GUID under this exact instance Device Parameters.',
    'Keep the original bth.inf/driver-store metadata for an exact-instance Device Manager rollback. Restoring the original Error state is not proof of hardware recovery.'
)
$result = [ordered] @{
    Schema = 1; UTC = [DateTime]::UtcNow.ToString('o'); Action = $Action
    ExpectedHardwareID = $ExpectedHardwareID
    DryRun = (-not $Apply); InstanceId = $TargetInstance; Status = [string] $device.Status
    FriendlyName = [string] $device.FriendlyName; Properties = $properties
    RegistryPath = ('HKLM:\' + $RegistryPath); DeviceInterfaceGUIDs = $guids
    DeviceInterfaceGUID = $singularGuid; DedicatedGuid = $AzkiInterfaceGuid
    DriverBindingPerformed = $false; RadioOpened = $false; HciCommandsSent = 0
    RegistryApplied = $false; ManualDriverSteps = $manualSteps; GuidChange = $null
}
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmss.fffffffZ') + '-' + [Guid]::NewGuid().ToString('N')
$backupPath = Join-Path $BackupDirectory ($stamp + '-before.json')

if ($Action -eq 'ConfigureGuid') {
    if ($guids.Exists -and [string] $guids.Kind -cne 'MultiString') {
        throw 'Existing DeviceInterfaceGUIDs is not REG_MULTI_SZ. Refusing to overwrite another representation.'
    }
    $values = @()
    if ($guids.Exists) { $values = @($guids.Value) }
    $afterValues = @($values)
    if ($afterValues -notcontains $AzkiInterfaceGuid) { $afterValues += $AzkiInterfaceGuid }
    $result.GuidChange = [ordered] @{ ValueName = $GuidValueName; Before = $guids
        After = [ordered] @{ Available = $true; Exists = $true; Kind = 'MultiString'; Value = @($afterValues) } }
}
elseif ($Action -eq 'RollbackGuid') {
    if (-not $RestoreFrom) { throw 'RollbackGuid requires an explicit prior ConfigureGuid backup/receipt.' }
    $restorePath = Assert-WithinScriptDirectory $RestoreFrom
    if (-not $restorePath.StartsWith($BackupDirectory.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Restore source must be inside this script backup directory.'
    }
    $saved = Get-Content -LiteralPath $restorePath -Raw | ConvertFrom-Json
    if ($saved.Schema -ne 1 -or $saved.Action -ne 'ConfigureGuid' -or
        -not [string]::Equals([string] $saved.InstanceId, $TargetInstance, [StringComparison]::OrdinalIgnoreCase) -or
        $null -eq $saved.GuidChange -or $saved.GuidChange.ValueName -cne $GuidValueName -or
        $saved.DedicatedGuid -cne $AzkiInterfaceGuid) {
        throw 'Restore source is not the exact-device Azki ConfigureGuid record.'
    }
    if (-not (Same-RegistryValue $guids $saved.GuidChange.After)) {
        throw 'Current GUID value differs from the recorded expected after-value; refusing to clobber another change.'
    }
    $before = $saved.GuidChange.Before
    if ($before.Exists -and [string] $before.Kind -cne 'MultiString') {
        throw 'Unsupported original value kind; manual review is required.'
    }
    $expectedValues = @()
    if ($before.Exists) { $expectedValues = @($before.Value) }
    if ($expectedValues -notcontains $AzkiInterfaceGuid) { $expectedValues += $AzkiInterfaceGuid }
    $expectedChange = [ordered] @{ Available = $true; Exists = $true; Kind = 'MultiString'; Value = @($expectedValues) }
    if (-not (Same-RegistryValue $expectedChange $saved.GuidChange.After)) {
        throw 'Restore record is not the exact append-only dedicated Azki GUID change.'
    }
    $result.GuidChange = [ordered] @{ ValueName = $GuidValueName; Before = $guids; After = $before; RestoreSource = $restorePath }
}

$result['BackupFile'] = $backupPath
Write-NewJson $backupPath $result

if ($Apply) {
    # Normal interactive driver binding must already have happened; this script
    # cannot select, install, remove, restart or re-enumerate any driver/device.
    if (-not $properties.HardwareIds.Available -or
        @($properties.HardwareIds.Value) -notcontains $ExpectedHardwareID) {
        throw ('The exact reviewed hardware ID was not confirmed. Backup: ' + $backupPath)
    }
    if ($Action -eq 'ConfigureGuid') {
        if (-not $properties.Service.Available -or [string] $properties.Service.Value -ine 'WinUSB' -or
            -not $properties.DriverInfPath.Available -or [string] $properties.DriverInfPath.Value -ine 'winusb.inf' -or
            -not $properties.DriverProvider.Available -or [string] $properties.DriverProvider.Value -notmatch '^Microsoft(?: Corporation)?$' -or
            [string] $device.Status -ine 'OK') {
            throw ('ConfigureGuid Apply requires this exact healthy device already using Microsoft inbox WinUSB. Backup: ' + $backupPath)
        }
        $systemDriver = Join-Path ([Environment]::SystemDirectory) 'drivers\WinUSB.sys'
        $signature = Get-AuthenticodeSignature -LiteralPath $systemDriver
        if ([string] $signature.Status -ine 'Valid' -or $null -eq $signature.SignerCertificate -or
            [string] $signature.SignerCertificate.Subject -notmatch '(?:^|,\s*)O=Microsoft Corporation(?:,|$)') {
            throw ('Microsoft inbox WinUSB signature could not be validated; no registry change. Backup: ' + $backupPath)
        }
    }
    elseif (-not $properties.Service.Available -or
            @('WinUSB', 'BTHUSB') -notcontains [string] $properties.Service.Value -or
            -not $properties.DriverInfPath.Available -or
            @('winusb.inf', 'bth.inf') -notcontains [string] $properties.DriverInfPath.Value) {
        throw ('RollbackGuid requires the exact known WinUSB or original BTHUSB binding; current status may be Error. Backup: ' + $backupPath)
    }
    if (-not (Same-RegistryValue (Read-RegistryValue $GuidValueName) $result.GuidChange.Before)) {
        throw 'GUID value changed since backup; refusing the write.'
    }
    if ($PSCmdlet.ShouldProcess(('HKLM:\' + $RegistryPath + '\' + $GuidValueName), $Action)) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,
                                                        [Microsoft.Win32.RegistryView]::Registry64)
        try {
            # Create only this already-verified instance's Device Parameters.
            $instanceKey = $base.OpenSubKey(('SYSTEM\CurrentControlSet\Enum\' + $TargetInstance), $true)
            if ($null -eq $instanceKey) { throw 'Exact instance registry key disappeared.' }
            try {
                $key = $instanceKey.CreateSubKey('Device Parameters')
                try {
                    $after = $result.GuidChange.After
                    if ($after.Exists) {
                        $key.SetValue($GuidValueName, [string[]] @($after.Value), [Microsoft.Win32.RegistryValueKind]::MultiString)
                    }
                    else { $key.DeleteValue($GuidValueName, $false) }
                }
                finally { $key.Dispose() }
            }
            finally { $instanceKey.Dispose() }
        }
        finally { $base.Dispose() }
        $result.RegistryApplied = $true
        $result.DryRun = $false
        $result['PostWriteValue'] = Read-RegistryValue $GuidValueName
        if (-not (Same-RegistryValue $result.PostWriteValue $result.GuidChange.After)) {
            throw ('Post-write verification failed; original backup: ' + $backupPath)
        }
        # GUID registration may need a normal user-controlled unplug/replug.
        # Do not restart/re-enumerate the device or any Bluetooth service here.
        $result['ReplugMayBeRequired'] = $true
        Write-NewJson (Join-Path $BackupDirectory ($stamp + '-applied.json')) $result
    }
}

[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
ConvertTo-Json -InputObject $result -Depth 20
