#!/bin/sh
# Build the minimal static FFmpeg that ImView links for media playback.
#
# Run this from an MSVC x64 developer environment (vcvars64.bat) inside a POSIX
# shell (MSYS2, Git Bash, or the bundled busybox sh) with GNU make and nasm on
# PATH. The 3rd/tools folder contains Windows builds of make and nasm.
#
#   tools/build_ffmpeg.sh [build-dir]
#
# The default build directory is build/ffmpeg-build, which is where CMake looks
# through IMVIEW_FFMPEG_ROOT. Set IMVIEW_FFMPEG_RECONFIGURE=1 to rerun configure.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
source_dir="$root_dir/3rd/ffmpeg"
build_dir=${1:-"$root_dir/build/ffmpeg-build"}

# The checked-in tools are Windows executables; putting them on PATH also makes
# nasm available to the FFmpeg x86 assembly step.
if [ -d "$root_dir/3rd/tools" ]; then
    PATH="$root_dir/3rd/tools:$PATH"
    export PATH
fi

# Prefer the bundled GNU make when the caller did not choose one. On Windows it
# needs a POSIX shell through MAKESHELL; MSYS2/Git Bash provide SHELL.
if [ -z "${MAKE:-}" ] && [ -x "$root_dir/3rd/tools/gnumake-4.4.1-x64.exe" ]; then
    MAKE="$root_dir/3rd/tools/gnumake-4.4.1-x64.exe"
fi
MAKE=${MAKE:-make}
MAKE_SHELL=${MAKESHELL:-${SHELL:-}}

mkdir -p "$build_dir"
cd "$build_dir"

if [ ! -f ffbuild/config.mak ] ||
   [ ! -f libswresample/swresample.lib ] ||
   [ ! -f .imview-external-subtitles ] ||
   [ "${IMVIEW_FFMPEG_RECONFIGURE:-0}" = "1" ]; then
    "$source_dir/configure" \
        --toolchain=msvc \
        --arch=x86_64 \
        --disable-everything \
        --disable-autodetect \
        --disable-debug \
        --disable-programs \
        --disable-doc \
        --disable-network \
        --disable-avdevice \
        --disable-avfilter \
        --disable-encoders \
        --disable-muxers \
        --disable-devices \
        --disable-filters \
        --enable-static \
        --disable-shared \
        --extra-cflags=/MD \
        --enable-swscale \
        --enable-swresample \
        --enable-protocol=file \
        --enable-demuxer=mov,matroska,avi,flv,asf,mpegts,mpegps,ogg,gif,rm,srt,ass,webvtt,sami,microdvd,subviewer,subviewer1,realtext \
        --enable-decoder=h264,hevc,vp8,vp9,av1,mpeg4,mpeg2video,mpeg1video,msmpeg4v3,wmv1,wmv2,wmv3,vc1,flv,mjpeg,gif,h263,prores,theora,vp6,vp6a,vp6f,rv30,rv40,dvvideo \
        --enable-decoder=aac,aac_latm,mp3,mp2,ac3,eac3,dca,truehd,opus,vorbis,flac,alac,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le,pcm_mulaw,pcm_alaw,wmav1,wmav2,wmapro,cook,amrnb,amrwb,nellymoser,adpcm_ima_wav,adpcm_ms \
        --enable-decoder=ass,subrip,webvtt,movtext,text,microdvd,sami,realtext,subviewer,subviewer1 \
        --enable-parser=h264,hevc,vp8,vp9,av1,mpeg4video,mpegvideo,mjpeg,flv,vc1,theora,aac,aac_latm,ac3,mpegaudio,opus,vorbis,flac,dca
fi

# allformats.o includes the generated demuxer_list.c, but its Makefile
# dependency does not track a configure-time list change. Rebuild it so a
# reconfigure that adds a demuxer cannot silently link the old list.
rm -f "$build_dir/libavformat/allformats.o"

if [ -n "$MAKE_SHELL" ]; then
    MAKESHELL="$MAKE_SHELL" "$MAKE" -j"${NUMBER_OF_PROCESSORS:-4}" SHELL="$MAKE_SHELL"
else
    "$MAKE" -j"${NUMBER_OF_PROCESSORS:-4}"
fi

# Marks a build that includes the sidecar subtitle demuxers. Existing build
# directories from before this feature therefore reconfigure once.
touch "$build_dir/.imview-external-subtitles"
