param([string]$PackageDirectory = 'out/dist')
# Publish only a verified package set, keeping the release private until ALL
# assets and their GitHub SHA-256 digests have been checked. No installer runs.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
$client = $null
try {
    $version = [regex]::Match((Get-Content CMakeLists.txt -Raw), 'project\(NexPlay VERSION ([0-9.]+)').Groups[1].Value
    $tag = "v$version"
    $remote = (& git -c "safe.directory=$root" remote get-url origin).Trim()
    if ($LASTEXITCODE -ne 0 -or $remote -notin @('https://github.com/Zimonxx/nexplay.git','https://github.com/Zimonxx/nexplay')) { throw 'Unexpected release repository' }
    $commit = (& git -c "safe.directory=$root" rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[a-f0-9]{40}$') { throw 'Cannot resolve release commit' }
    $dirty = & git -c "safe.directory=$root" status --porcelain --untracked-files=no
    if ($dirty) { throw 'Commit all tracked changes before publishing' }
    $directory = [IO.Path]::GetFullPath($PackageDirectory)
    $names = @("NexPlay-$version-Setup-windows-x64.exe", "NexPlay-$version-windows-x64.zip", "NexPlay-$version-FFmpeg-source.zip", 'SHA256SUMS.txt')
    $hashes = @{}
    $sums = Get-Content -LiteralPath (Join-Path $directory 'SHA256SUMS.txt')
    foreach ($name in $names) {
        $path = Join-Path $directory $name
        $hashes[$name] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($name -ne 'SHA256SUMS.txt' -and $sums -cnotcontains ($hashes[$name] + '  ' + $name)) { throw "Package checksum mismatch: $name" }
    }
    # Inno Setup pads this version-resource string with spaces.
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $directory $names[0])).ProductVersion.Trim() -ne $version) { throw 'Installer version mismatch' }
    $token = $env:GH_TOKEN
    if (!$token) { $token = $env:GITHUB_TOKEN }
    if (!$token) {
        # Keep credentials in memory only; never log the helper's output.
        $credential = "protocol=https`nhost=github.com`n`n" | & git -c "safe.directory=$root" credential fill
        if ($LASTEXITCODE -ne 0) { throw 'GitHub authentication unavailable' }
        foreach ($line in $credential) { if ($line.StartsWith('password=')) { $token = $line.Substring(9) } }
        $credential = $null
    }
    if (!$token) { throw 'GitHub authentication unavailable' }
    Add-Type -AssemblyName System.Net.Http
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $client = [Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromMinutes(10)
    $client.DefaultRequestHeaders.Authorization = [Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $token)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('NexPlay-Release/1.0')
    $client.DefaultRequestHeaders.Add('X-GitHub-Api-Version', '2022-11-28')
    $token = $null
    $api = 'https://api.github.com/repos/Zimonxx/nexplay'
    function Request([string]$Method, [string]$Url, $Body = $null, [bool]$AllowMissing = $false) {
        $request = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::new($Method), $Url)
        if ($Body) { $request.Content = [Net.Http.StringContent]::new(($Body | ConvertTo-Json -Depth 10), [Text.Encoding]::UTF8, 'application/json') }
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        try {
            if ($AllowMissing -and [int]$response.StatusCode -eq 404) { return $null }
            if (!$response.IsSuccessStatusCode) { throw "GitHub API $Method failed with HTTP $([int]$response.StatusCode)" }
            return ($response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json)
        } finally { $response.Dispose(); $request.Dispose() }
    }
    # Don't publish an unpushed/local-only build.
    $branch = Request GET "$api/commits/main"
    if ($branch.sha -cne $commit) { throw 'Push the release commit to main before publishing' }
    $existingTag = Request GET "$api/git/ref/tags/$tag" $null $true
    if ($existingTag) {
        $tagCommit = Request GET "$api/commits/$tag"
        if ($tagCommit.sha -cne $commit) { throw 'Version tag already points at another commit' }
    } else { $null = Request POST "$api/git/refs" @{ref="refs/tags/$tag"; sha=$commit} }
    $release = Request GET "$api/releases/tags/$tag" $null $true
    if ($release -and !$release.draft) { throw 'Release is already public; refusing to replace it' }
    if (!$release) {
        $release = Request POST "$api/releases" @{tag_name=$tag; target_commitish=$commit; name="NexPlay $tag";
            body=(Get-Content release/RELEASE-NOTES.md -Raw); draft=$true; prerelease=$false}
    }
    foreach ($name in $names) {
        $existing = @($release.assets | Where-Object name -ceq $name)
        if ($existing.Count) {
            if ($existing.Count -ne 1 -or $existing[0].digest -cne ('sha256:' + $hashes[$name])) { throw "Conflicting draft asset: $name" }
            continue
        }
        $file = [IO.File]::OpenRead((Join-Path $directory $name))
        $content = [Net.Http.StreamContent]::new($file)
        $content.Headers.ContentType = [Net.Http.Headers.MediaTypeHeaderValue]::new('application/octet-stream')
        $url = "https://uploads.github.com/repos/Zimonxx/nexplay/releases/$($release.id)/assets?name=$([uri]::EscapeDataString($name))"
        try {
            $response = $client.PostAsync($url, $content).GetAwaiter().GetResult()
            try {
                if (!$response.IsSuccessStatusCode) { throw "Upload failed: $name (HTTP $([int]$response.StatusCode))" }
                $asset = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
                if ($asset.digest -cne ('sha256:' + $hashes[$name])) { throw "Uploaded digest mismatch: $name" }
                Write-Output "Verified asset: $name"
            } finally { $response.Dispose() }
        } finally { $content.Dispose(); $file.Dispose() }
    }
    $release = Request GET "$api/releases/$($release.id)"
    foreach ($name in $names) {
        $asset = @($release.assets | Where-Object name -ceq $name)
        if ($asset.Count -ne 1 -or $asset[0].state -ne 'uploaded' -or $asset[0].digest -cne ('sha256:' + $hashes[$name])) { throw "Incomplete release: $name" }
    }
    $release = Request PATCH "$api/releases/$($release.id)" @{draft=$false; make_latest='true'}
    Write-Output "Published: $($release.html_url)"
} finally {
    if ($client) { $client.Dispose() }
    Pop-Location
}
