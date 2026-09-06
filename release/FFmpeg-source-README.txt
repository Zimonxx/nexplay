Corresponding source for NexPlay's FFmpeg distribution
=====================================================

FFmpeg 9.0.1 is unmodified. Its original source archive and the exact NVIDIA
nv-codec-headers files used for the build are included. The source licenses and
copyright notices are in that archive and in the header files. This build is
LGPL 2.1 or later; GPL and nonfree components are disabled.

Build on Windows x64 with Visual Studio 2022 C++ Build Tools + Windows SDK,
PowerShell 7 and Git for Windows (including Bash).

1. Extract this entire source ZIP.
2. Create out/downloads and copy ffmpeg-9.0.1.tar.xz into it.
3. Run: pwsh -File release/Build-FFmpeg.ps1
4. The result is in out/dependencies/ffmpeg/bin.

The script downloads pinned, SHA256-verified GNU Make, pkgconf and NASM build
tools; their source projects are https://www.gnu.org/software/make/,
https://github.com/pkgconf/pkgconf and https://www.nasm.us/. Build tools are not
redistributed in the NexPlay binary package. The MSYS runtime comes with Git
for Windows. FFmpeg does not require that runtime at execution time.

Configuration used for the supplied binaries is in configuration/. MSVC uses
the static C runtime (-MT), so no Visual C++ redistributable is needed. The
FFmpeg libraries themselves are shared DLLs and may be replaced. No NVIDIA
driver binaries are shipped. The driver is loaded from the user's system.

FFmpeg upstream source: https://ffmpeg.org/releases/ffmpeg-9.0.1.tar.xz
SHA256: cf38e0e28c7e5605942c4a77755349b0145804a397af37eb1fb4c77cb237f635
NexPlay source and release assets: https://github.com/Zimonxx/nexplay
