# Developer deployment helper. Everyday launch uses Roclint.exe / the shortcuts;
# this does not claim to be a signed, clean-machine release installer.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$StagedDirectory,
    [string]$InstallDirectory = (Join-Path $env:LOCALAPPDATA 'Programs\Roclint'),
    [switch]$DesktopShortcut
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $StagedDirectory).Path
$destination = [IO.Path]::GetFullPath($InstallDirectory)
if ($source.TrimEnd('\') -eq $destination.TrimEnd('\')) {
    throw 'The staging and installation directories must differ.'
}
foreach ($required in @('roclint_host.exe', 'Qt6Core.dll', 'platforms\qwindows.dll',
                         'runtime\roclint_granite_worker.exe', 'runtime\julia_worker.py')) {
    if (!(Test-Path -LiteralPath (Join-Path $source $required) -PathType Leaf)) {
        throw "Incomplete staging directory: $required is missing."
    }
}
$installedExecutable = Join-Path $destination 'Roclint.exe'
foreach ($process in Get-Process -Name Roclint -ErrorAction SilentlyContinue) {
    if ($process.Path -eq $installedExecutable) {
        throw 'Quit Roclint from its tray menu before updating the installed copy.'
    }
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
# Copy the complete deployed runtime, omitting development-only executables.
foreach ($entry in Get-ChildItem -LiteralPath $source) {
    if ($entry.Name -in @('roclint_host.exe', 'roclint_gui_cua_test.exe', 'report-verification-probe.exe')) { continue }
    Copy-Item -LiteralPath $entry.FullName -Destination $destination -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $source 'roclint_host.exe') -Destination $installedExecutable -Force
$licenses = Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../app/qml/shell/assets/Inter-LICENSE.txt') -Destination (Join-Path $licenses 'Inter-OFL.txt') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../app/qml/shell/assets/icons/LICENSE') -Destination (Join-Path $licenses 'Phosphor-MIT.txt') -Force
$shell = New-Object -ComObject WScript.Shell
$programs = [Environment]::GetFolderPath('Programs')
$shortcutPaths = @(Join-Path $programs 'Roclint.lnk')
if ($DesktopShortcut) { $shortcutPaths += Join-Path ([Environment]::GetFolderPath('DesktopDirectory')) 'Roclint.lnk' }
foreach ($shortcutPath in $shortcutPaths) {
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $installedExecutable
    $shortcut.WorkingDirectory = $destination
    $shortcut.Arguments = ''
    $shortcut.IconLocation = "$installedExecutable,0"
    $shortcut.Description = 'Roclint — prompt assistant'
    $shortcut.Save()
}
[pscustomobject]@{ Executable = $installedExecutable; Shortcuts = $shortcutPaths; ReleaseInstaller = $false } | ConvertTo-Json
