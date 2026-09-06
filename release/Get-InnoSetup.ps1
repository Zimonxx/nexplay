$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$compiler = Join-Path $root 'out/tools/InnoSetup/ISCC.exe'
if (!(Test-Path $compiler)) {
    $download = Join-Path $root 'out/downloads/innosetup-6.7.3.exe'
    New-Item -ItemType Directory -Force (Split-Path $download) | Out-Null
    if (!(Test-Path $download)) {
        Invoke-WebRequest 'https://github.com/jrsoftware/issrc/releases/download/is-6_7_3/innosetup-6.7.3.exe' -OutFile $download
    }
    if ((Get-FileHash $download).Hash -ne '9C73C3BAE7ED48D44112A0F48E66742C00090BDB5BEF71D9D3C056C66E97B732') { throw 'Inno Setup SHA256 mismatch' }
    if ((Get-AuthenticodeSignature $download).Status -ne 'Valid') { throw 'Inno Setup publisher signature is invalid' }
    $directory = Split-Path $compiler
    $process = Start-Process $download -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /CURRENTUSER /DIR=`"$directory`" /NOICONS" -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Cannot install packaging tool: $($process.ExitCode)" }
}
if (!(Test-Path $compiler)) { throw 'ISCC.exe is missing' }
return $compiler
