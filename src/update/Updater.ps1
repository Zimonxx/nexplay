param(
    [ValidateSet('Check','Stage','Apply','Library')][string]$Mode = 'Check',
    [string]$Job, [string]$Target, [string]$CurrentVersion,
    [int]$ParentId = 0
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NexPlayUpdateProcess {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder name, ref int size);
}
'@

function Write-State([string]$Phase, [int]$Percent = 0, [string]$Version = '', [string]$Detail = '') {
    $text = "$Phase|$Percent|$Version|" + ($Detail -replace '[\r\n|]', ' ')
    # Readers tolerate a transient sharing violation; never parse a partial write.
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    $stream = $null
    for ($retry = 0; $retry -lt 50; $retry++) {
        try { $stream = [IO.File]::Open((Join-Path $Job 'status'), 'Create', 'Write', 'None'); break }
        catch [IO.IOException] { Start-Sleep -Milliseconds 10 }
    }
    if (!$stream) { throw 'Cannot write update status' }
    try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
}
function Convert-Version([string]$Value) {
    if ($Value -cnotmatch '^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$') {
        throw 'Invalid stable version'
    }
    return [version]$Value
}
function Assert-Url([uri]$Url) {
    if ($Url.Scheme -ne 'https' -or $Url.Port -ne 443 -or $Url.UserInfo -or
        $Url.DnsSafeHost -notin @('api.github.com','github.com','release-assets.githubusercontent.com','objects.githubusercontent.com')) {
        throw 'Untrusted update URL'
    }
}
function Receive-File([string]$Url, [string]$Path, [long]$Limit, [bool]$Progress = $false) {
    $handler = New-Object Net.Http.HttpClientHandler
    $handler.AllowAutoRedirect = $false
    $client = New-Object Net.Http.HttpClient($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(45)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('NexPlay-Updater/1.0')
    $response = $null
    try {
        for ($i = 0; $i -lt 6; $i++) {
            Assert-Url ([uri]$Url)
            $response = $client.GetAsync($Url, [Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
            if ([int]$response.StatusCode -in @(301,302,303,307,308)) {
                $Url = (New-Object uri([uri]$Url, $response.Headers.Location)).AbsoluteUri
                $response.Dispose(); $response = $null
                continue
            }
            [void]$response.EnsureSuccessStatusCode()
            break
        }
        if (!$response -or [int]$response.StatusCode -ne 200) { throw 'Too many redirects' }
        $length = $response.Content.Headers.ContentLength
        if ($length -gt $Limit) { throw 'Update response too large' }
        $inputStream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $output = [IO.File]::Open($Path, 'CreateNew', 'Write', 'None')
        try {
            $buffer = New-Object byte[] 131072
            [long]$total = 0; $lastPercent = -1
            $deadline = [DateTime]::UtcNow.AddMinutes(15)
            while ($true) {
                $read = $inputStream.ReadAsync($buffer, 0, $buffer.Length)
                if (!$read.Wait(30000)) { throw 'Download timed out' }
                $count = $read.Result
                if (!$count) { break }
                $total += $count
                if ($total -gt $Limit -or [DateTime]::UtcNow -gt $deadline) { throw 'Download limit exceeded' }
                $output.Write($buffer, 0, $count)
                if ($Progress -and $length -gt 0) {
                    $percent = [int][Math]::Min(89, [Math]::Floor(90 * $total / $length))
                    if ($percent -ne $lastPercent) { Write-State 'downloading' $percent $script:release.version; $lastPercent = $percent }
                }
            }
            if ($length -and $total -ne $length) { throw 'Incomplete download' }
        } finally { $output.Dispose(); $inputStream.Dispose() }
    } finally { if ($response) { $response.Dispose() }; $client.Dispose(); $handler.Dispose() }
}
function Select-Release($Metadata, [string]$Installed) {
    $current = Convert-Version $Installed
    if ($Metadata.draft -or $Metadata.prerelease) { return $null }
    if ($Metadata.tag_name -cnotmatch '^v(.+)$') { throw 'Invalid release tag' }
    $version = $Matches[1]
    if ((Convert-Version $version) -le $current) { return $null }
    $name = "NexPlay-$version-windows-x64.zip"
    $assets = @($Metadata.assets | Where-Object { $_.name -ceq $name -and $_.state -eq 'uploaded' })
    if ($assets.Count -ne 1) { throw 'Release is not ready: portable ZIP missing' }
    $asset = $assets[0]
    $expectedUrl = "https://github.com/Zimonxx/nexplay/releases/download/v$version/$name"
    if ($asset.browser_download_url -cne $expectedUrl -or $asset.digest -cnotmatch '^sha256:([a-f0-9]{64})$' -or
        $asset.size -lt 1 -or $asset.size -gt 536870912) { throw 'Invalid release asset or SHA-256 digest' }
    return @{version=$version; url=$expectedUrl; hash=$asset.digest.Substring(7); size=[long]$asset.size}
}
function Assert-PlainPath([string]$Path) {
    # Reject junctions/symlinks in every existing ancestor, not just the last file.
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Update path contains a reparse point'
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }; $cursor = $parent
    }
}
function Get-RelativeName([string]$Name) {
    $name = $Name.Replace('\','/')
    if (!$name.StartsWith('NexPlay/', [StringComparison]::Ordinal)) { throw 'Invalid ZIP root' }
    $relative = $name.Substring(8)
    # Only package-owned paths, never recordings, settings or uninstaller state.
    if ($relative -notmatch '^(nexplay\.exe|LICENSE|README\.md|README\.txt|THIRD-PARTY-NOTICES\.txt|licenses/[A-Za-z0-9_.-]+\.txt|tools/ffmpeg/bin/[A-Za-z0-9_.-]+\.(exe|dll))$') {
        throw "Unexpected package path: $name"
    }
    return $relative
}
function Expand-Package([string]$Zip, [string]$Destination) {
    Assert-PlainPath $Destination
    if (Test-Path -LiteralPath $Destination) { throw 'Staging folder already exists' }
    $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        if ($archive.Entries.Count -gt 256) { throw 'Too many archive entries' }
        $seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $entries = @(); [long]$total = 0
        foreach ($entry in $archive.Entries) {
            if (($entry.ExternalAttributes -band 0x400) -ne 0 -or (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'Archive link rejected' }
            if ($entry.FullName.EndsWith('/')) {
                if ($entry.FullName -cnotin @('NexPlay/','NexPlay/licenses/','NexPlay/tools/','NexPlay/tools/ffmpeg/','NexPlay/tools/ffmpeg/bin/')) { throw 'Invalid archive directory' }
                continue
            }
            $relative = Get-RelativeName $entry.FullName
            if (!$seen.Add($relative)) { throw 'Duplicate archive entry' }
            $total += $entry.Length
            if ($total -gt 2147483648) { throw 'Expanded package too large' }
            $entries += @{entry=$entry; relative=$relative}
        }
        foreach ($required in @('nexplay.exe','tools/ffmpeg/bin/ffmpeg.exe','tools/ffmpeg/bin/ffprobe.exe','LICENSE')) {
            if (!$seen.Contains($required)) { throw "Missing package file: $required" }
        }
        [void][IO.Directory]::CreateDirectory($Destination)
        foreach ($item in $entries) {
            $path = Join-Path $Destination $item.relative
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($item.entry, $path, $false)
        }
        return @($entries | ForEach-Object { $_.relative })
    } finally { $archive.Dispose() }
}
function Assert-Application([string]$Directory, [string]$Version) {
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $Directory 'nexplay.exe'))
    if ($info.ProductName -cne 'NexPlay' -or $info.ProductVersion -cne $Version) { throw 'Application version mismatch' }
}
function Read-PackageFiles([string]$Path) {
    # Assign first, then enumerate explicitly. In Windows PowerShell 5.1,
    # @(Get-Content ... | ConvertFrom-Json) wraps the whole JSON array in one
    # element; binding that to string[] joins all filenames with spaces.
    $jsonArguments = @{InputObject=(Get-Content -LiteralPath $Path -Raw)}
    if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('NoEnumerate')) {
        $jsonArguments.NoEnumerate = $true
    }
    $decoded = ConvertFrom-Json @jsonArguments
    if ($decoded -isnot [array] -or $decoded.Count -lt 1 -or $decoded.Count -gt 256) {
        throw 'Invalid package file list'
    }
    $seen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $decoded) {
        if ($name -isnot [string]) { throw 'Invalid package filename' }
        [void](Get-RelativeName "NexPlay/$name")
        if (!$seen.Add($name.Replace('\','/'))) { throw 'Duplicate package filename' }
    }
    foreach ($name in $decoded) { Write-Output $name }
}
function Install-Package([string]$Source, [string]$Destination, [string]$Backup, [string[]]$Files) {
    Assert-PlainPath $Destination
    Assert-PlainPath $Source
    Assert-PlainPath $Backup
    if (Test-Path -LiteralPath $Backup) { throw 'Backup already exists' }
    $changed = New-Object 'Collections.Generic.List[object]'
    try {
        # Preflight ALL targets before changing any file. Never follow a junction.
        foreach ($name in $Files) {
            [void](Get-RelativeName "NexPlay/$name")
            Assert-PlainPath (Join-Path $Destination $name)
            Assert-PlainPath (Join-Path $Source $name)
        }
        foreach ($name in $Files) {
            $to = Join-Path $Destination $name
            $from = Join-Path $Source $name
            $old = Join-Path $Backup $name
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($to))
            $existed = [IO.File]::Exists($to)
            $pending = $to + '.nexplay-update-' + [guid]::NewGuid().ToString('N')
            try {
                [IO.File]::Copy($from, $pending, $false)
                if ($existed) {
                    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($old))
                    [IO.File]::Copy($to, $old, $false)
                    # Atomic replacement on the target volume; the old file remains
                    # intact even if another process locks it during the update.
                    [IO.File]::Replace($pending, $to, [NullString]::Value)
                } else { [IO.File]::Move($pending, $to) }
                $changed.Add(@{path=$to; backup=$old; existed=$existed})
            } finally {
                if ([IO.File]::Exists($pending)) { [IO.File]::Delete($pending) }
            }
        }
    } catch {
        $failure = $_
        $rollbackFailed = $false
        for ($i = $changed.Count - 1; $i -ge 0; $i--) {
            $item = $changed[$i]
            try {
                if ($item.existed) {
                    # Portable installs may live on a different volume than LocalAppData.
                    $restore = $item.path + '.nexplay-update-' + [guid]::NewGuid().ToString('N')
                    try {
                        [IO.File]::Copy($item.backup, $restore, $false)
                        [IO.File]::Replace($restore, $item.path, [NullString]::Value)
                    } finally { if ([IO.File]::Exists($restore)) { [IO.File]::Delete($restore) } }
                }
                elseif ([IO.File]::Exists($item.path)) { [IO.File]::Delete($item.path) }
            } catch { $rollbackFailed = $true }
        }
        if ($rollbackFailed) { throw "Update failed; recovery files are in $Backup" }
        throw $failure
    }
}
function Apply-Update([string]$Target, [string]$CurrentVersion, [int]$ParentId,
                      [string]$InstanceName = 'Local\NexPlay.Application') {
    # Acquire the process handle BEFORE acknowledging readiness; no PID reuse race.
    $parent = [Diagnostics.Process]::GetProcessById($ParentId)
    $Target = [IO.Path]::GetFullPath($Target)
    # MainModule cannot inspect a 64-bit parent from a 32-bit PowerShell host.
    # QueryFullProcessImageName works across architectures and pins the handle.
    $parentName = New-Object Text.StringBuilder 32768
    $parentNameSize = $parentName.Capacity
    if (![NexPlayUpdateProcess]::QueryFullProcessImageName($parent.Handle, 0, $parentName, [ref]$parentNameSize) -or
        ![string]::Equals($parentName.ToString(), (Join-Path $Target 'nexplay.exe'), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Parent application mismatch'
    }
    $release = Get-Content -LiteralPath (Join-Path $Job 'release.json') -Raw | ConvertFrom-Json
    Assert-PlainPath $Target
    Assert-Application $Target $CurrentVersion
    Assert-Application (Join-Path $Job 'stage') $release.version
    if ((Convert-Version $release.version) -le (Convert-Version $CurrentVersion)) { throw 'Downgrade rejected' }
    # Reject a malformed manifest while the old application is still open.
    $files = @(Read-PackageFiles (Join-Path $Job 'files.json'))
    Write-State 'restarting' 100 $release.version
    if (!$parent.WaitForExit(600000)) { throw 'Application did not close; update cancelled' }
    $parent.Dispose()
    $created = $false
    $guard = New-Object Threading.Mutex($false, $InstanceName, [ref]$created)
    if (!$created) { $guard.Dispose(); throw 'NexPlay was opened again; update cancelled' }
    try {
        Install-Package (Join-Path $Job 'stage') $Target (Join-Path $Job 'backup') $files
        # Installed copies retain their existing uninstaller and uninstall metadata.
        $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{FF9670F0-8335-4E43-A0CD-8D0EB9BF6F42}_is1'
        try {
            if (Test-Path $key) {
                $installed = Get-ItemProperty $key
                if ($installed.InstallLocation -and $installed.InstallLocation.TrimEnd('\') -ieq $Target.TrimEnd('\')) {
                    Set-ItemProperty $key DisplayVersion $release.version -ErrorAction SilentlyContinue
                }
            }
        } catch { } # Optional metadata must not prevent restarting a verified update.
        Write-State 'installed' 100 $release.version
    } finally { $guard.Dispose() }
    # This is the user-requested interactive application restart, not a helper.
    Start-Process (Join-Path $Target 'nexplay.exe') -WorkingDirectory $Target -WindowStyle Normal
}
if ($Mode -eq 'Library') { return }
try {
    $Job = [IO.Path]::GetFullPath($Job)
    Assert-PlainPath $Job
    $metadataPath = Join-Path $Job 'release.json'
    if ($Mode -eq 'Check') {
        Write-State 'checking'
        $responsePath = Join-Path $Job 'response.json'
        Receive-File 'https://api.github.com/repos/Zimonxx/nexplay/releases/latest' $responsePath 4194304
        $release = Select-Release (Get-Content -LiteralPath $responsePath -Raw | ConvertFrom-Json) $CurrentVersion
        if (!$release) { Write-State 'current'; exit 0 }
        $release | ConvertTo-Json | Set-Content -LiteralPath $metadataPath -Encoding UTF8
        Write-State 'available' 0 $release.version
    } elseif ($Mode -eq 'Stage') {
        $script:release = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
        if ((Convert-Version $release.version) -le (Convert-Version $CurrentVersion)) { throw 'Downgrade rejected' }
        $zip = Join-Path $Job 'package.zip'
        Write-State 'downloading' 0 $release.version
        Receive-File $release.url $zip 536870912 $true
        Write-State 'verifying' 90 $release.version
        if ((Get-Item -LiteralPath $zip).Length -ne $release.size -or (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ine $release.hash) { throw 'SHA-256 verification failed' }
        $stage = Join-Path $Job 'stage'
        $files = @(Expand-Package $zip $stage)
        Assert-Application $stage $release.version
        Write-State 'verifying' 95 $release.version
        $probe = Start-Process (Join-Path $stage 'nexplay.exe') -ArgumentList '--verify-installation' -WindowStyle Hidden -PassThru
        try {
            if (!$probe.WaitForExit(60000)) { $probe.Kill(); throw 'Package verification timed out' }
            if ($probe.ExitCode -ne 0) { throw 'Package verification failed' }
        } finally { $probe.Dispose() }
        ConvertTo-Json -InputObject @($files) | Set-Content -LiteralPath (Join-Path $Job 'files.json') -Encoding UTF8
        Write-State 'ready' 100 $release.version
    } else {
        Apply-Update $Target $CurrentVersion $ParentId
    }
} catch {
    try { Write-State 'error' 0 '' $_.Exception.Message } catch { }
    if ($Mode -eq 'Apply') {
        Add-Type -AssemblyName System.Windows.Forms
        [void][Windows.Forms.MessageBox]::Show("Aktualizacja nie powiodla sie: $($_.Exception.Message)`n`nPliki odzyskiwania i opis bledu: $Job", 'NexPlay - aktualizacja')
    }
    exit 1
}
