#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/bin:$PATH"
root="$(pwd)"
export PATH="$(cygpath -u "$NEXPLAY_COMPILER_BIN"):$root/out/tools/msys/usr/bin:$root/out/tools/nasm/nasm-2.16.03:$PATH"
prefix="$root/out/dependencies/ffmpeg"
# A changed component list must never reuse objects from the previous configuration.
build="$root/out/dependencies/ffmpeg-build-$(sha256sum "$root/release/build-ffmpeg.sh" | cut -c1-12)"
mkdir -p "$build/pkgconfig" "$prefix"
sed "s|@@PREFIX@@|$root/third_party/nv-codec-headers|g" \
    third_party/nv-codec-headers/ffnvcodec.pc.in > "$build/pkgconfig/ffnvcodec.pc"
export PKG_CONFIG_PATH="$build/pkgconfig"
cd "$build"
"$root/out/dependencies/ffmpeg-9.0.1/configure" \
    --prefix="$prefix" --toolchain=msvc --arch=x86_64 --target-os=win64 \
    --enable-shared --disable-static --disable-debug --disable-doc \
    --disable-autodetect --disable-network --disable-gpl --disable-nonfree \
    --disable-everything --enable-ffmpeg --enable-ffprobe --disable-ffplay \
    --enable-ffnvcodec --enable-nvenc --enable-nvdec \
    --enable-hwaccel=h264_nvdec,hevc_nvdec \
    --enable-protocol=file,pipe \
    --enable-demuxer=mov,h264,hevc,pcm_s16le,image2,image2pipe,wav,matroska \
    --enable-muxer=mp4,mov,image2,image2pipe,null,pcm_s16le,adts,rawvideo,h264,wav \
    --enable-decoder=h264,hevc,aac,pcm_s16le,mjpeg,rawvideo,mpeg4,wrapped_avframe \
    --enable-encoder=h264_nvenc,aac,mjpeg,pcm_s16le,rawvideo,mpeg4,wrapped_avframe \
    --enable-parser=h264,hevc,aac,mjpeg,mpeg4video \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata,aac_adtstoasc \
    --enable-filter=amix,atrim,asetpts,concat,aresample,anull,aformat,format,trim,setpts,scale,pad,select,showinfo,fps,volume,split,asplit,settb,asettb,sine,testsrc2,anullsrc,null,adelay,apad \
    --enable-indev=lavfi --extra-cflags=-MT
make -j"${1:-4}"
make install
cp config.h ffbuild/config.mak "$prefix/"
"$prefix/bin/ffmpeg.exe" -buildconf > "$prefix/BUILD-CONFIG.txt" 2>&1
"$prefix/bin/ffmpeg.exe" -L > "$prefix/LICENSE.txt" 2>&1
