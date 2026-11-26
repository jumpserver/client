param(
    [Parameter(Mandatory = $true)][string]$Dist,
    [ValidateSet('amd64', '386')][string]$Arch = 'amd64'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# This deliberately changes protocol registration. Never run on a developer's
# workstation or a persistent/self-hosted runner with real user configuration.
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'Installer integration checks require a disposable GitHub-hosted Windows runner.'
}
$InstallDir = Join-Path $(if ($Arch -eq 'amd64') { $env:ProgramW6432 } else { ${env:ProgramFiles(x86)} }) 'OSMClient'
if (-not $InstallDir -or $InstallDir -eq 'OSMClient') { $InstallDir = Join-Path $env:ProgramFiles 'OSMClient' }
$Shortcut = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'OSMClient.lnk'
$DesktopShortcut = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'OSMClient.lnk'
$LegacyShortcut = Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'OSM Client Qt.lnk'
$ProtocolKey = 'Software\Classes\jms'
$CommandKey = "$ProtocolKey\shell\open\command"
$Expected = '"' + (Join-Path $InstallDir 'client.exe') + '" "%1"'
$OtherCommand = '"C:\Synthetic-OtherClient\client.exe" "%1"'
$Registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry32)

function Read-Command {
    $key = $Registry.OpenSubKey($CommandKey)
    if ($null -eq $key) { return $null }
    try { return $key.GetValue('') } finally { $key.Dispose() }
}

function Invoke-CheckedProcess([string]$File, [string]$Arguments, [int[]]$Allowed = @(0)) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $File
    $start.Arguments = $Arguments
    $start.UseShellExecute = $false
    $process = [System.Diagnostics.Process]::Start($start)
    try {
        if (-not $process.WaitForExit(180000)) {
            $process.Kill()
            throw "Installer process timed out: $([System.IO.Path]::GetFileName($File))"
        }
        if ($Allowed -notcontains $process.ExitCode) {
            throw "Installer process failed: $([System.IO.Path]::GetFileName($File)) (exit $($process.ExitCode))"
        }
    } finally { $process.Dispose() }
}

$NsisKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\OSMClient'
$MsiKey = 'Software\FIT2CLOUD\OSMClient'

function Test-RegistryValue([string]$Path, [string]$Name) {
    # PowerShell variable names are case-insensitive: keep the opened key apart from the path.
    $subKey = $Registry.OpenSubKey($Path)
    if ($null -eq $subKey) { return $false }
    try { return $null -ne $subKey.GetValue($Name) } finally { $subKey.Dispose() }
}

function Get-ClientProcesses {
    $prefix = $InstallDir.TrimEnd('\') + '\'
    @(Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
    })
}

function Show-MsiLog([string]$Log) {
    if (-not (Test-Path -LiteralPath $Log)) { Write-Host "MSI log not found: $Log"; return }
    $lines = @(Get-Content -LiteralPath $Log)
    Write-Host "==== MSI log excerpt: $([System.IO.Path]::GetFileName($Log)) ($($lines.Count) lines)"
    $failure = @(0..($lines.Count - 1) | Where-Object { $lines[$_] -match 'Return value 3' })
    foreach ($index in $failure | Select-Object -First 2) {
        $start = [Math]::Max(0, $index - 40)
        $lines[$start..$index] | ForEach-Object { Write-Host $_ }
        Write-Host '----'
    }
    $lines | Where-Object { $_ -match 'CloseInstalledClients|RemoveNsisInstall|NSISINSTALL|MSIINSTALL|AppSearch|Error 17|CustomAction .* returned|Product: OSMClient|Doing action: InstallFiles' } |
        Select-Object -First 60 | ForEach-Object { Write-Host $_ }
    Write-Host '==== end of MSI log excerpt'
}

function Invoke-Msi([string]$Arguments, [string]$Log) {
    try {
        Invoke-CheckedProcess 'msiexec.exe' "$Arguments /L*v `"$Log`"" @(0, 3010)
    } catch {
        Show-MsiLog $Log
        throw
    }
}

function Start-Client {
    $process = Start-Process -FilePath (Join-Path $InstallDir 'client.exe') -PassThru
    Start-Sleep -Seconds 3
    if ($process.HasExited) {
        Write-Warning "client.exe exited before replacement (exit $($process.ExitCode)); running-client handling is not exercised."
    }
    return $process
}

function Show-ReplaceDiagnostics([string]$MsiLog) {
    Write-Host '==== replacement diagnostics'
    Get-ClientProcesses | ForEach-Object { Write-Host "running: $($_.ProcessId) $($_.ExecutablePath)" }
    Write-Host "EXE registration: $(Test-RegistryValue $NsisKey 'InstallLocation'); MSI marker: $(Test-RegistryValue $MsiKey 'WindowsInstaller')"
    Write-Host "Uninstall.exe present: $(Test-Path -LiteralPath (Join-Path $InstallDir 'Uninstall.exe'))"
    $logs = @((Join-Path $env:SystemRoot 'Temp\OSMClient-setup.log'),
              (Join-Path $env:SystemRoot 'SystemTemp\OSMClient-setup.log'),
              (Join-Path $env:TEMP 'OSMClient-setup.log')) | Select-Object -Unique
    foreach ($log in $logs) {
        if (Test-Path -LiteralPath $log) {
            Write-Host "-- $log"
            Get-Content -LiteralPath $log | ForEach-Object { Write-Host $_ }
        } else { Write-Host "-- $log (missing)" }
    }
    if ($MsiLog) { Show-MsiLog $MsiLog }
}

function Assert-ClientClosed($Process, [string]$Step, [string]$MsiLog = '') {
    $Process.Refresh()
    $problem = if (-not $Process.HasExited) { "$Step did not close the running client." }
               elseif (@(Get-ClientProcesses).Count) { "$Step left processes running from the install folder." }
    if ($problem) {
        Show-ReplaceDiagnostics $MsiLog
        throw $problem
    }
}

function Verify-Payload {
    $helper = if ($Arch -eq 'amd64') { 'JumpServerClient.exe' } else { 'JumpServerClient32.exe' }
    foreach ($relative in @('client.exe', 'platforms\qwindows.dll', "bin\windows\$helper", 'bin\windows\putty.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $InstallDir $relative) -PathType Leaf)) {
            throw "Installed payload missing $relative"
        }
    }
    if (-not (Test-Path -LiteralPath $Shortcut -PathType Leaf)) { throw 'Installed OSMClient shortcut is missing.' }
    if (-not (Test-Path -LiteralPath $DesktopShortcut -PathType Leaf)) { throw 'Installed desktop shortcut is missing.' }
    if (Test-Path -LiteralPath $LegacyShortcut) { throw 'Installation retained the legacy application shortcut.' }
    if ((Read-Command) -cne $Expected) { throw 'Installed jms command does not target this client.' }
    $env:QT_QPA_PLATFORM = 'windows'
    Remove-Item Env:QT_PLUGIN_PATH, Env:QT_QPA_PLATFORM_PLUGIN_PATH, Env:QML2_IMPORT_PATH -ErrorAction SilentlyContinue
    $env:PATH = "$InstallDir;$env:SystemRoot\System32;$env:SystemRoot"
    Invoke-CheckedProcess (Join-Path $InstallDir 'client.exe') '--smoke-test'
}

try {
    $existingProtocol = $Registry.OpenSubKey($ProtocolKey)
    if ($null -ne $existingProtocol) {
        $existingProtocol.Dispose()
        throw 'Refusing to replace pre-existing jms registration on this runner.'
    }
    if (Test-Path -LiteralPath $InstallDir) { throw 'Refusing to reuse an existing application directory.' }
    $destination = (Resolve-Path -LiteralPath $Dist).Path
    foreach ($extension in @('exe', 'msi')) {
        $packages = @(Get-ChildItem -LiteralPath $destination -Filter "OSMClient-*-windows-$Arch.$extension")
        if ($packages.Count -ne 1) { throw "Expected exactly one $Arch $extension package in $destination" }
        $package = $packages[0].FullName
        foreach ($takeover in @($false, $true)) {
            Write-Host "Verifying $extension installation (other handler takeover: $takeover)"
            if ($extension -eq 'msi') {
                $log = Join-Path $destination "verify-$extension-$takeover-install.log"
                Invoke-Msi "/i `"$package`" /qn /norestart" $log
            } else {
                Invoke-CheckedProcess $package '/S'
            }
            Verify-Payload
            if ($takeover) {
                $key = $Registry.CreateSubKey($CommandKey)
                try { $key.SetValue('', $OtherCommand) } finally { $key.Dispose() }
                $key = $Registry.CreateSubKey($ProtocolKey)
                try { $key.SetValue('OtherClientMarker', 'synthetic') } finally { $key.Dispose() }
            }
            if ($extension -eq 'msi') {
                $log = Join-Path $destination "verify-$extension-$takeover-uninstall.log"
                Invoke-Msi "/x `"$package`" /qn /norestart" $log
            } else {
                # _?= keeps the uninstaller in this process so exit means completion.
                Invoke-CheckedProcess (Join-Path $InstallDir 'Uninstall.exe') "/S _?=$InstallDir"
                $uninstaller = Join-Path $InstallDir 'Uninstall.exe'
                if (Test-Path -LiteralPath $uninstaller) { Remove-Item -LiteralPath $uninstaller }
                if ((Test-Path -LiteralPath $InstallDir) -and @(Get-ChildItem -LiteralPath $InstallDir -Force).Count -eq 0) {
                    Remove-Item -LiteralPath $InstallDir
                }
            }
            if (Test-Path -LiteralPath (Join-Path $InstallDir 'client.exe')) { throw 'Uninstall left the application executable.' }
            if (Test-Path -LiteralPath $Shortcut) { throw 'Uninstall left the OSMClient shortcut.' }
            if (Test-Path -LiteralPath $DesktopShortcut) { throw 'Uninstall left the desktop shortcut.' }
            if ($takeover) {
                if ((Read-Command) -cne $OtherCommand) { throw 'Uninstall modified another client jms registration.' }
                $key = $Registry.OpenSubKey($ProtocolKey)
                try {
                    if ($key.GetValue('OtherClientMarker') -ne 'synthetic') { throw 'Uninstall removed another client protocol values.' }
                } finally { $key.Dispose() }
                $Registry.DeleteSubKeyTree($ProtocolKey, $false)
            } elseif ($null -ne (Read-Command)) {
                throw 'Uninstall retained its own jms command.'
            }
        }
    }

    # Replacement of an existing installation while its client is running:
    # EXE -> MSI, MSI -> EXE, then EXE -> EXE of the same version.
    $exePackage = @(Get-ChildItem -LiteralPath $destination -Filter "OSMClient-*-windows-$Arch.exe")[0].FullName
    $msiPackage = @(Get-ChildItem -LiteralPath $destination -Filter "OSMClient-*-windows-$Arch.msi")[0].FullName
    Write-Host 'Verifying replacement of existing installations with a running client'
    Invoke-CheckedProcess $exePackage '/S'
    Verify-Payload
    $client = Start-Client
    $log = Join-Path $destination 'verify-replace-exe-with-msi.log'
    Invoke-Msi "/i `"$msiPackage`" /qn /norestart" $log
    Assert-ClientClosed $client 'MSI replacing the EXE installation' $log
    if (Test-RegistryValue $NsisKey 'InstallLocation') { throw 'MSI install did not remove the EXE installation.' }
    if (-not (Test-RegistryValue $MsiKey 'WindowsInstaller')) { throw 'MSI install marker is missing after replacing the EXE installation.' }
    if (Test-Path -LiteralPath (Join-Path $InstallDir 'Uninstall.exe')) { throw 'MSI replacement left the EXE uninstaller behind.' }
    Verify-Payload

    $client = Start-Client
    Invoke-CheckedProcess $exePackage '/S'
    Assert-ClientClosed $client 'EXE replacing the MSI installation'
    if (Test-RegistryValue $MsiKey 'WindowsInstaller') { throw 'EXE install did not remove the MSI installation.' }
    if (-not (Test-RegistryValue $NsisKey 'InstallLocation')) { throw 'EXE install registration is missing after replacing the MSI installation.' }
    Verify-Payload

    $client = Start-Client
    Invoke-CheckedProcess $exePackage '/S'
    Assert-ClientClosed $client 'EXE reinstalling over the EXE installation'
    Verify-Payload

    $client = Start-Client
    Invoke-CheckedProcess (Join-Path $InstallDir 'Uninstall.exe') "/S _?=$InstallDir"
    Assert-ClientClosed $client 'EXE uninstall'
    $uninstaller = Join-Path $InstallDir 'Uninstall.exe'
    if (Test-Path -LiteralPath $uninstaller) { Remove-Item -LiteralPath $uninstaller }
    if ((Test-Path -LiteralPath $InstallDir) -and @(Get-ChildItem -LiteralPath $InstallDir -Force).Count -eq 0) {
        Remove-Item -LiteralPath $InstallDir
    }
    if (Test-Path -LiteralPath (Join-Path $InstallDir 'client.exe')) { throw 'Final uninstall left the application executable.' }
    if (Test-RegistryValue $NsisKey 'InstallLocation') { throw 'Final uninstall left the EXE registration.' }
    if ($null -ne (Read-Command)) { throw 'Final uninstall retained its own jms command.' }
    Write-Host 'EXE/MSI installed payload, replacement and jms ownership checks passed.'
} finally {
    $Registry.Dispose()
}
