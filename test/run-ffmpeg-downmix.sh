#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
ndk_dir=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
: "${ndk_dir:?Set ANDROID_NDK_HOME to an Android NDK installation}"
ffmpeg_dir=${FFMPEG_ANDROID_DIR:-"$repo_dir/../prebuilt/ffmpeg/dist-full-armeabi-v7a"}
adb_cmd=${ADB:-adb}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/avos-ffmpeg-downmix.XXXXXX")
device_dir=/data/local/tmp/avos-ffmpeg-downmix-$$-$RANDOM
cleanup() {
    "$adb_cmd" shell rm -rf "$device_dir" >/dev/null 2>&1 || true
    rm -rf "$build_dir"
}
trap cleanup EXIT

# Run on an ARM32-capable Android device using the same FFmpeg as the APK.
# ANDROID_SERIAL selects the device if more than one is connected.
"$ndk_dir/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi23-clang" \
    -std=gnu11 -O1 -g -ffunction-sections -fdata-sections \
    -DCONFIG_RELEASE -DCONFIG_STREAM -DCONFIG_FFMPEG_AUDIO -DSTANDALONE \
    -I"$repo_dir/Include" -I"$ffmpeg_dir/include" \
    "$repo_dir/test/ffmpeg_downmix.c" -L"$ffmpeg_dir/lib" \
    -Wl,--gc-sections -lswresample -lavutil -lm -o "$build_dir/ffmpeg-downmix"
"$adb_cmd" shell mkdir -p "$device_dir"
"$adb_cmd" push "$build_dir/ffmpeg-downmix" \
    "$ffmpeg_dir/lib/libswresample.so" "$ffmpeg_dir/lib/libavutil.so" "$device_dir/"
"$adb_cmd" shell "LD_LIBRARY_PATH=$device_dir $device_dir/ffmpeg-downmix"
