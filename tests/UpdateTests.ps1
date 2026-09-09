param([string]$OutputDirectory = "$PSScriptRoot/../out/update-tests")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../src/update/Updater.ps1" -Mode Library
$root = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ([guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Assert $rejected $Message
}
function Metadata([string]$Version) {
    return [pscustomobject]@{tag_name="v$Version"; draft=$false; prerelease=$false; assets=@(
        [pscustomobject]@{name="NexPlay-$Version-windows-x64.zip"; state='uploaded'; size=100;
        digest=('sha256:' + ('a' * 64)); browser_download_url="https://github.com/Zimonxx/nexplay/releases/download/v$Version/NexPlay-$Version-windows-x64.zip"})}
}
Assert ((Select-Release (Metadata '0.10.0') '0.2.3').version -eq '0.10.0') 'Numeric version ordering'
Assert ($null -eq (Select-Release (Metadata '0.2.3') '0.2.3')) 'Same version offered'
Assert ($null -eq (Select-Release (Metadata '0.2.2') '0.2.3')) 'Downgrade offered'
$metadata = Metadata '0.3.0'; $metadata.prerelease = $true
Assert ($null -eq (Select-Release $metadata '0.2.3')) 'Prerelease offered'
$metadata.prerelease = $false; $metadata.draft = $true
Assert ($null -eq (Select-Release $metadata '0.2.3')) 'Draft offered'
foreach ($version in @('0.3.0-beta','1.2','01.2.3','1.2.3.4','../3.0')) {
    Reject { Convert-Version $version } "Invalid version accepted: $version"
}
$metadata = Metadata '0.3.0'; $metadata.assets[0].digest = ''
Reject { Select-Release $metadata '0.2.3' } 'Missing hash accepted'
$metadata = Metadata '0.3.0'; $metadata.assets[0].browser_download_url = 'https://example.com/update.zip'
Reject { Select-Release $metadata '0.2.3' } 'Foreign asset accepted'
$metadata = Metadata '0.3.0'; $metadata.assets += $metadata.assets[0]
Reject { Select-Release $metadata '0.2.3' } 'Duplicate asset accepted'
foreach ($url in @('http://github.com/file','https://github.com.evil.test/a','https://github.com:444/a','https://user@github.com/a','file:///C:/a')) {
    Reject { Assert-Url ([uri]$url) } "Untrusted URL accepted: $url"
}
Assert-Url ([uri]'https://release-assets.githubusercontent.com/a')
foreach ($name in @('NexPlay/../nexplay.exe','NexPlay/tools/ffmpeg/bin/../../evil.dll','C:/NexPlay/nexplay.exe',
    'NexPlay/nexplay.exe:stream','NexPlay/Clips/clip.mp4','NexPlay/unins000.exe','Other/nexplay.exe')) {
    Reject { Get-RelativeName $name } "Unsafe path accepted: $name"
}
function Make-Zip([string]$Name, [string[]]$Extras) {
    $path = Join-Path $root $Name
    $zip = [IO.Compression.ZipFile]::Open($path, 'Create')
    try {
        foreach ($entry in (@('NexPlay/nexplay.exe','NexPlay/tools/ffmpeg/bin/ffmpeg.exe','NexPlay/tools/ffmpeg/bin/ffprobe.exe','NexPlay/LICENSE') + $Extras)) {
            $writer = New-Object IO.StreamWriter($zip.CreateEntry($entry).Open())
            try { $writer.Write('fixture') } finally { $writer.Dispose() }
        }
    } finally { $zip.Dispose() }
    return $path
}
$valid = Make-Zip 'valid.zip' @()
$files = @(Expand-Package $valid (Join-Path $root 'expanded'))
Assert ($files.Count -eq 4) 'Valid package failed'
Reject { Expand-Package (Make-Zip 'slip.zip' @('NexPlay/../escape.exe')) (Join-Path $root 'slip') } 'Zip slip accepted'
Reject { Expand-Package (Make-Zip 'duplicate.zip' @('NexPlay/NEXPLAY.EXE')) (Join-Path $root 'duplicate') } 'Case collision accepted'
Assert (!(Test-Path (Join-Path $root 'escape.exe'))) 'Escaped staging directory'

$source = Join-Path $root 'source'; $target = Join-Path $root 'target'
[void][IO.Directory]::CreateDirectory($source); [void][IO.Directory]::CreateDirectory($target)
foreach ($name in @('nexplay.exe','README.md','LICENSE')) { [IO.File]::WriteAllText((Join-Path $source $name), 'new') }
foreach ($name in @('nexplay.exe','README.md','unins000.exe','my-recording.mp4','preferences.json')) { [IO.File]::WriteAllText((Join-Path $target $name), 'old') }
$files = @('nexplay.exe','LICENSE','README.md')
$lock = [IO.File]::Open((Join-Path $target 'README.md'), 'Open', 'Read', 'None')
try {
    Reject { Install-Package $source $target (Join-Path $root 'backup-failed') $files } 'Locked file not rejected'
} finally { $lock.Dispose() }
Assert ([IO.File]::ReadAllText((Join-Path $target 'nexplay.exe')) -eq 'old') 'Rollback did not restore old executable'
Assert (!(Test-Path (Join-Path $target 'LICENSE'))) 'Rollback left a new file behind'
Assert (@(Get-ChildItem $target -Filter '*.nexplay-update-*').Count -eq 0) 'Partial files left behind'
Install-Package $source $target (Join-Path $root 'backup-success') $files
Assert ([IO.File]::ReadAllText((Join-Path $target 'nexplay.exe')) -eq 'new') 'Executable not updated'
foreach ($name in @('unins000.exe','my-recording.mp4','preferences.json')) {
    Assert ([IO.File]::ReadAllText((Join-Path $target $name)) -eq 'old') "User/uninstaller file changed: $name"
}
$linked = Join-Path $root 'linked'
New-Item -ItemType Junction -Path $linked -Target $target | Out-Null
Reject { Install-Package $source $linked (Join-Path $root 'backup-link') $files } 'Junction target accepted'
Write-Output 'PASS: stable releases, versions, trusted URLs, digests, archive traversal/duplicates, atomic replacement, locked-file rollback, user data and junction protection.'

# Exercise the actual parent handshake, wait, replacement and restart using two
# tiny synthetic GUI-subsystem executables. They show no UI and access no audio,
# recording, settings, registry or real application mutex.
$app = Join-Path $root 'restart app with spaces'
$jobDir = Join-Path $root 'restart job'
$stage = Join-Path $jobDir 'stage'
[void][IO.Directory]::CreateDirectory($app); [void][IO.Directory]::CreateDirectory($stage)
foreach ($version in @('0.2.2','0.2.3')) {
    $directory = if ($version -eq '0.2.2') { $app } else { $stage }
    $class = 'Fixture' + $version.Replace('.','')
    $code = @"
using System;
using System.IO;
using System.Reflection;
using System.Threading;
[assembly: AssemblyProduct("NexPlay")]
[assembly: AssemblyInformationalVersion("$version")]
[assembly: AssemblyVersion("$version.0")]
public class $class {
    public static void Main(string[] args) {
        var folder = AppDomain.CurrentDomain.BaseDirectory;
        if (args.Length > 0) {
            using (var instance = new Mutex(false, args[0])) {
                File.WriteAllText(Path.Combine(folder, "parent-ready"), "ready");
                var deadline = DateTime.UtcNow.AddSeconds(90);
                while (!File.Exists(Path.Combine(folder, "exit-parent")) && DateTime.UtcNow < deadline) Thread.Sleep(20);
            }
        } else File.WriteAllText(Path.Combine(folder, "restarted"), "$version");
    }
}
"@
    Add-Type -TypeDefinition $code -OutputAssembly (Join-Path $directory 'nexplay.exe') -OutputType WindowsApplication
}
@{version='0.2.3'} | ConvertTo-Json | Set-Content (Join-Path $jobDir 'release.json') -Encoding UTF8
@('nexplay.exe') | ConvertTo-Json | Set-Content (Join-Path $jobDir 'files.json') -Encoding UTF8
[IO.File]::WriteAllText((Join-Path $app 'my-recording.mp4'), 'keep')
$instanceName = 'Local\NexPlay.UpdateTest.' + [guid]::NewGuid().ToString('N')
$parentProcess = Start-Process (Join-Path $app 'nexplay.exe') -ArgumentList $instanceName -WindowStyle Hidden -PassThru
$helperProcess = $null
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (!(Test-Path (Join-Path $app 'parent-ready')) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 20 }
    Assert (Test-Path (Join-Path $app 'parent-ready')) 'Fixture parent did not start'
    $script = Join-Path $PSScriptRoot 'UpdateApplyFixture.ps1'
    $arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$script`" -TestJob `"$jobDir`" -TestTarget `"$app`" -TestParentId $($parentProcess.Id) -TestInstance $instanceName"
    $helperProcess = Start-Process "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    $status = ''
    while ($status -notlike 'restarting*' -and [DateTime]::UtcNow -lt $deadline -and !$helperProcess.HasExited) {
        if (Test-Path (Join-Path $jobDir 'status')) { try { $status = [IO.File]::ReadAllText((Join-Path $jobDir 'status')) } catch { } }
        Start-Sleep -Milliseconds 20
    }
    if (Test-Path (Join-Path $jobDir 'status')) { $status = [IO.File]::ReadAllText((Join-Path $jobDir 'status')) }
    Assert ($status -like 'restarting*') "Helper handshake failed: $status"
    Assert-Application $app '0.2.2'
    Assert (!$parentProcess.HasExited) 'Helper terminated the application instead of waiting'
    [IO.File]::WriteAllText((Join-Path $app 'exit-parent'), 'exit')
    Assert ($helperProcess.WaitForExit(20000)) 'Apply helper did not finish'
    $status = [IO.File]::ReadAllText((Join-Path $jobDir 'status'))
    Assert ($helperProcess.ExitCode -eq 0) "Apply helper failed: $status"
    Assert ($status -like 'installed|100|0.2.3*') "Incorrect completion status: $status"
    Assert-Application $app '0.2.3'
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (!(Test-Path (Join-Path $app 'restarted')) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 20 }
    Assert ([IO.File]::ReadAllText((Join-Path $app 'restarted')) -eq '0.2.3') 'Updated application did not restart'
    Assert ([IO.File]::ReadAllText((Join-Path $app 'my-recording.mp4')) -eq 'keep') 'Restart changed user files'
    Write-Output 'PASS: real parent handshake, graceful wait, versioned executable replacement and automatic restart; isolated from running NexPlay.'
} finally {
    [IO.File]::WriteAllText((Join-Path $app 'exit-parent'), 'exit')
    if ($helperProcess) {
        if (!$helperProcess.HasExited) { $helperProcess.Kill() }
        $helperProcess.Dispose()
    }
    [void]$parentProcess.WaitForExit(5000)
    $parentProcess.Dispose()
}
