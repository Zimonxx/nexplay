param(
    [string]$Executable = 'out/build/windows-x64-release/Release/nexplay.exe',
    [string]$FFmpegDirectory = 'out/dependencies/ffmpeg',
    [string]$OutputDirectory = 'out/dist',
    [switch]$TestInstaller
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    $version = [regex]::Match((Get-Content CMakeLists.txt -Raw), 'project\(NexPlay VERSION ([0-9.]+)').Groups[1].Value
    $Executable = (Resolve-Path $Executable).Path
    $FFmpegDirectory = (Resolve-Path $FFmpegDirectory).Path
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo($Executable).ProductVersion -ne $version) { throw 'Executable/version mismatch' }
    foreach ($tool in @('ffmpeg.exe', 'ffprobe.exe')) {
        if (!(Test-Path "$FFmpegDirectory/bin/$tool")) { throw "Missing $tool" }
    }
    $configuration = Get-Content "$FFmpegDirectory/BUILD-CONFIG.txt" -Raw
    $license = Get-Content "$FFmpegDirectory/LICENSE.txt" -Raw
    if ($configuration -notmatch '--disable-gpl' -or $configuration -notmatch '--disable-nonfree' -or $license -notmatch 'Lesser General Public License') {
        throw 'Expected the reviewed LGPL FFmpeg build'
    }
    # Fresh staging prevents obsolete files from leaking into a release.
    $stage = Join-Path $root ("out/package-stage-" + [guid]::NewGuid().ToString('N'))
    $package = Join-Path $stage 'NexPlay'
    New-Item -ItemType Directory -Force "$package/tools/ffmpeg/bin", "$package/licenses", $OutputDirectory | Out-Null
    Copy-Item $Executable "$package/nexplay.exe"
    Copy-Item LICENSE, README.md, release/README.txt $package
    Get-Content release/README.txt -Raw | Set-Content "$package/README.txt" -Encoding utf8BOM
    Copy-Item release/THIRD-PARTY-NOTICES.txt $package
    Copy-Item "$FFmpegDirectory/bin/*.exe", "$FFmpegDirectory/bin/*.dll" "$package/tools/ffmpeg/bin"
    Copy-Item out/dependencies/ffmpeg-9.0.1/COPYING.LGPLv2.1 "$package/licenses/FFmpeg-LGPL-2.1.txt"
    Copy-Item "$FFmpegDirectory/BUILD-CONFIG.txt" "$package/licenses/FFmpeg-BUILD-CONFIG.txt"
    $check = Start-Process "$package/nexplay.exe" -ArgumentList '--verify-installation' -WindowStyle Hidden -PassThru -Wait
    if ($check.ExitCode -ne 0) { throw "Package smoke test failed: $($check.ExitCode)" }
    & "$PSScriptRoot/Generate-BrandAssets.ps1" | Out-Host
    $compiler = & "$PSScriptRoot/Get-InnoSetup.ps1"
    $defines = @("/DAppVersion=$version", "/DPackageDir=$package", "/DOutputDir=$OutputDirectory", "/DIconFile=$root/out/brand/nexplay.ico", "/DBrandPng=$root/out/brand/nexplay.png")
    & $compiler @defines "$PSScriptRoot/NexPlay.iss"
    if ($LASTEXITCODE) { throw 'Installer compilation failed' }
    Compress-Archive -Path $package -DestinationPath "$OutputDirectory/NexPlay-$version-windows-x64.zip" -Force
    $sourcePackage = Join-Path $stage 'FFmpeg-source'
    New-Item -ItemType Directory -Force "$sourcePackage/release", "$sourcePackage/third_party/nv-codec-headers", "$sourcePackage/configuration" | Out-Null
    Copy-Item out/downloads/ffmpeg-9.0.1.tar.xz $sourcePackage
    Copy-Item release/Build-FFmpeg.ps1, release/build-ffmpeg.sh "$sourcePackage/release"
    Copy-Item release/FFmpeg-source-README.txt "$sourcePackage/README.txt"
    Copy-Item third_party/nv-codec-headers/include, third_party/nv-codec-headers/ffnvcodec.pc.in "$sourcePackage/third_party/nv-codec-headers" -Recurse
    Copy-Item "$FFmpegDirectory/config.h", "$FFmpegDirectory/config.mak", "$FFmpegDirectory/BUILD-CONFIG.txt", "$FFmpegDirectory/LICENSE.txt" "$sourcePackage/configuration"
    Compress-Archive -Path $sourcePackage -DestinationPath "$OutputDirectory/NexPlay-$version-FFmpeg-source.zip" -Force
    Get-ChildItem $OutputDirectory -File | Where-Object Name -in @("NexPlay-$version-Setup-windows-x64.exe", "NexPlay-$version-windows-x64.zip", "NexPlay-$version-FFmpeg-source.zip") |
        Sort-Object Name | ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName).Hash.ToLowerInvariant(), $_.Name } |
        Set-Content "$OutputDirectory/SHA256SUMS.txt" -Encoding ascii
    if ($TestInstaller) {
        & $compiler @defines '/DTestInstall=1' "$PSScriptRoot/NexPlay.iss"
        if ($LASTEXITCODE) { throw 'Test installer compilation failed' }
        & "$PSScriptRoot/Test-Package.ps1" -Installer "$OutputDirectory/NexPlay-TestSetup.exe"
    }
    Write-Output "Packages: $OutputDirectory"
} finally { Pop-Location }
