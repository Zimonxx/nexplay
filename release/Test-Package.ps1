param([Parameter(Mandatory)][string]$Installer)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$Installer = (Resolve-Path $Installer).Path
if ([IO.Path]::GetFileName($Installer) -ne 'NexPlay-TestSetup.exe') { throw 'Use the isolated test installer, not the public installer.' }
$registryPath = 'Software\NexPlay.PackageTest'
if ([Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($registryPath)) { throw 'A previous test registry key exists; refusing to overwrite it.' }
$testRoot = Join-Path $root ("out/installer-smoke-" + [guid]::NewGuid().ToString('N'))
$installation = [IO.Path]::GetFullPath((Join-Path $testRoot 'NexPlay with spaces'))
if (!$installation.StartsWith([IO.Path]::GetFullPath((Join-Path $root 'out')) + [IO.Path]::DirectorySeparatorChar)) { throw 'Unsafe test directory' }
New-Item -ItemType Directory -Force $installation | Out-Null
function Run-Checked([string]$File, [string]$Arguments) {
    $process = Start-Process $File -ArgumentList $Arguments -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "$File failed: $($process.ExitCode)" }
}
$testKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($registryPath)
$runKey = $testKey.CreateSubKey('Run')
$originalPath = $env:PATH
try {
    $options = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /NOICONS /TASKS=`"`" /DIR=`"$installation`" /LOG=`"$testRoot/install.log`""
    Run-Checked $Installer $options
    if ($runKey.GetValue('NexPlay.PackageTest')) { throw 'Fresh install enabled autostart' }
    $busy = [Threading.Mutex]::new($false, 'Local\NexPlay.PackageTest')
    try {
        $blocked = Start-Process $Installer -ArgumentList $options -WindowStyle Hidden -Wait -PassThru
        if ($blocked.ExitCode -eq 0) { throw 'Installer did not protect a running application' }
    } finally { $busy.Dispose() }
    # No developer PATH: the app must find its bundled media tools.
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    Run-Checked "$installation/nexplay.exe" '--verify-installation'
    $env:PATH = $originalPath
    $testKey.SetValue('KeepSettings', 'preserve')
    'Preserve this untracked user file' | Set-Content "$installation/keep-user-file.txt"
    $runKey.SetValue('NexPlay.PackageTest', '"C:\Old NexPlay\nexplay.exe" --autostart')
    Run-Checked $Installer $options
    $expected = '"' + ($installation + '\nexplay.exe') + '" --autostart'
    if ($runKey.GetValue('NexPlay.PackageTest') -ne $expected) { throw 'Upgrade did not preserve/update autostart' }
    $uninstaller = (Resolve-Path "$installation/unins000.exe").Path
    if (!(Split-Path $uninstaller).Equals($installation, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected uninstaller path' }
    Run-Checked $uninstaller "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=`"$testRoot/uninstall.log`""
    if (Test-Path "$installation/nexplay.exe") { throw 'Uninstall left the program behind' }
    if (!(Test-Path "$installation/keep-user-file.txt") -or $testKey.GetValue('KeepSettings') -ne 'preserve') { throw 'Uninstall removed user data' }
    if ($runKey.GetValue('NexPlay.PackageTest')) { throw 'Uninstall left its autostart entry' }
    Write-Output 'PASS: install, running-app protection, bundled tools without PATH, upgrade, autostart preservation, uninstall and user-data preservation.'
} finally {
    $env:PATH = $originalPath
    $runKey.Dispose()
    $testKey.Dispose()
    # Only this test's isolated, previously absent registry namespace.
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($registryPath, $false)
}
