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
                Invoke-CheckedProcess 'msiexec.exe' "/i `"$package`" /qn /norestart /L*v `"$log`"" @(0, 3010)
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
                Invoke-CheckedProcess 'msiexec.exe' "/x `"$package`" /qn /norestart /L*v `"$log`"" @(0, 3010)
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
    Write-Host 'EXE/MSI installed payload and jms ownership checks passed.'
} finally {
    $Registry.Dispose()
}
