param([int]$Jobs = 4)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$downloads = Join-Path $root 'out/downloads'
$dependencies = Join-Path $root 'out/dependencies'
$toolRoot = Join-Path $root 'out/tools'
New-Item -ItemType Directory -Force $downloads, $dependencies, $toolRoot | Out-Null
function Fetch-Verified($Name, $Url, $Hash) {
    $target = Join-Path $downloads $Name
    if (!(Test-Path $target)) { Invoke-WebRequest $Url -OutFile $target }
    if ((Get-FileHash $target -Algorithm SHA256).Hash -ne $Hash) {
        throw "SHA256 mismatch: $Name. Remove this download and retry."
    }
    return $target
}
$source = Fetch-Verified 'ffmpeg-9.0.1.tar.xz' 'https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz' 'CF38E0E28C7E5605942C4A77755349B0145804A397AF37EB1FB4C77CB237F635'
$make = Fetch-Verified 'make-4.4.1-3.pkg.tar.zst' 'https://repo.msys2.org/msys/x86_64/make-4.4.1-3-x86_64.pkg.tar.zst' 'AF0BDBA17F06FE037F0194069ADAA31A8FE45F1A11381501896AEA1FAE37BD5D'
$pkgconf = Fetch-Verified 'pkgconf-2.5.1-1.pkg.tar.zst' 'https://repo.msys2.org/msys/x86_64/pkgconf-2.5.1-1-x86_64.pkg.tar.zst' '38EFD4928AC0CB06E9BD558BAA533FC923C0592115BF9BD44970250F3AE2CB7E'
$nasm = Fetch-Verified 'nasm-2.16.03-win64.zip' 'https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip' '3EE4782247BCB874378D02F7EAB4E294A84D3D15F3F6EE2DE2F47A46AA7226E6'
if (!(Test-Path "$dependencies/ffmpeg-9.0.1/configure")) {
    & tar -xf $source -C $dependencies
    if ($LASTEXITCODE) { throw 'Cannot extract FFmpeg source' }
}
New-Item -ItemType Directory -Force "$toolRoot/msys", "$toolRoot/nasm" | Out-Null
foreach ($archive in @($make, $pkgconf)) {
    & tar -xf $archive -C "$toolRoot/msys"
    if ($LASTEXITCODE) { throw 'Cannot extract build tool' }
}
Expand-Archive $nasm "$toolRoot/nasm" -Force
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio C++ Build Tools (x64) first.' }
$envLines = & cmd.exe /d /c "call `"$vs\Common7\Tools\VsDevCmd.bat`" -arch=x64 >nul && set"
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        if ($matches[1] -ieq 'Path') { $compilerPath = $matches[2] }
        else { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
    }
}
$gitRoot = Split-Path (Split-Path (Get-Command git.exe).Source)
$bash = Join-Path $gitRoot 'bin/bash.exe'
if (!(Test-Path $bash)) { throw 'Git for Windows with Bash is required.' }
# MSVC link.exe must precede MSYS link.exe; GNU find must precede Windows find.exe.
$compilerBin = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64'
$env:NEXPLAY_COMPILER_BIN = $compilerBin
$env:PATH = "$compilerBin;$toolRoot/msys/usr/bin;$toolRoot/nasm/nasm-2.16.03;$gitRoot/usr/bin;$compilerPath"
Push-Location $root
try {
    & $bash --noprofile --norc release/build-ffmpeg.sh $Jobs
    if ($LASTEXITCODE) { throw "FFmpeg build failed: $LASTEXITCODE" }
} finally { Pop-Location }
