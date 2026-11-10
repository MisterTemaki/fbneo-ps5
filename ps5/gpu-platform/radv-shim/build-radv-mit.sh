#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# RADV (Mesa's Vulkan driver for the PS5, MIT) built against the MIT stand-ins for the payload SDK fork's three
# ps5platform headers (include/ps5platform/: agc.h, kernel.h, videoout.h) instead of the fork's (GPL-3.0-or-later).
#
# Everything else is as the existing archive was built (its PROVENANCE.txt): the same Mesa tree (PS5_Mesa 7b59ef27
# with PS5-ECAM's patches, git tree $TREE), the same meson options and cross file, the same build id, the SDK fork's
# other headers. The SDK is a copy of the fork's installed one with ps5platform/ and libps5platform.a taken out and
# these headers put in, so the fork's can't be found.
#
#   build-radv-mit.sh <out-dir>
# Environment (defaults: this machine's rsdeps):
#   VULKAN    PS5_Vulkan checkout (its .deps/native/ps5-payload-sdk, tooling/radv/ps5-cross.ini, .deps/work/)
#   SRC       the patched Mesa source tree (its .tree file must say $TREE)
#   OPTIONS   the release's meson options (radv-ps5-release.radv-options)
#   CLC_BIN   the folder with the host mesa_clc / vtn_bindgen2
#   BUILD     the build folder (removed at the end, with the SDK copy, unless KEEP_BUILD=1)
#   JOBS      ninja's parallel jobs (4)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build-radv-mit.sh <out-dir>}
VULKAN=${VULKAN:-/home/claude/work/rsdeps/PS5_Vulkan}
work=$VULKAN/.deps/work
SRC=${SRC:-$work/ps5cemu-radv-src}
OPTIONS=${OPTIONS:-$work/radv-ps5-release.radv-options}
CLC_BIN=${CLC_BIN:-$work/radv-clc-bin}
BUILD=${BUILD:-$work/radv-mit-build}
TREE=${TREE:-db544590b101bbf7f8940b0ec2feac6ee3e655c8}
BUILD_ID=${BUILD_ID:-7b59ef27c1b09b9671bc4153c41940c3155c3af2}
fork_sdk=$VULKAN/.deps/native/ps5-payload-sdk

[[ $(cat "$SRC/.tree") == "$TREE" ]] || { echo "$SRC is not Mesa tree $TREE" >&2; exit 2; }
[[ -f $OPTIONS && -x $CLC_BIN/mesa_clc ]] || { echo "no meson options ($OPTIONS) or mesa_clc ($CLC_BIN)" >&2; exit 2; }
mkdir -p "$out/lib"
out=$(cd "$out" && pwd)

# the SDK: the fork's, without its platform layer, with the MIT headers
sdk=$out/sdk
rm -rf "$sdk"
cp -a "$fork_sdk" "$sdk"
rm -rf "$sdk/target/include/ps5platform" "$sdk/target/lib/libps5platform.a"
mkdir -p "$sdk/target/include/ps5platform"
cp "$here"/include/ps5platform/{agc.h,kernel.h,videoout.h} "$sdk/target/include/ps5platform/"
printf "[constants]\nsdk = '%s'\n" "$sdk" >"$out/cross-constants.ini"

read -ra options <"$OPTIONS"
export PATH=$CLC_BIN:$PATH
rm -rf "$BUILD"
meson setup "$BUILD" "$SRC" --cross-file "$out/cross-constants.ini" --cross-file "$VULKAN/tooling/radv/ps5-cross.ini" \
	"${options[@]}" -Dradv-build-id="$BUILD_ID" >"$out/meson-setup.log" 2>&1 || { tail -20 "$out/meson-setup.log" >&2; exit 1; }
ninja -C "$BUILD" -j "${JOBS:-4}" src/amd/vulkan/libvulkan_radeon.a >"$out/ninja.log" 2>&1 ||
	{ grep -E "error|FAILED" "$out/ninja.log" | head -20 >&2; exit 1; }
cp "$BUILD/src/amd/vulkan/libvulkan_radeon.a" "$out/lib/libvulkan_radeon.ps5.a"

# what a rebuild needs: the options, the headers (also in the SDK copy), the cross file
cp "$OPTIONS" "$out/radv-options"
mkdir -p "$out/include/ps5platform"
cp "$here"/include/ps5platform/*.h "$out/include/ps5platform/"
# Consumers compile against these headers through SLANG_GPU_RADV_MIT/include.
# Ship the exact Vulkan declarations used by this Mesa build, as the upstream RADV recipe does.
cp -R "$SRC/include/vulkan" "$out/include/"
if [[ -d $SRC/include/vk_video ]]; then
	cp -R "$SRC/include/vk_video" "$out/include/"
fi
{
	echo "RADV built against ps5-native-runtime-mit's MIT ps5platform headers, by platform/radv-shim/build-radv-mit.sh"
	echo "mesa revision: 7b59ef27c1b09b9671bc4153c41940c3155c3af2 (PS5_Mesa)"
	echo "mesa tree: $TREE (that revision with PS5-ECAM 5.1.0's patches/mesa 0006, 0007, 0008, 0009)"
	echo "build id: $BUILD_ID"
	echo "sdk: the payload SDK fork's install ($(cat "$fork_sdk/.ps5-sdk-revision" 2>/dev/null || echo '?')) without ps5platform/ and libps5platform.a"
	echo "ps5platform headers (MIT):"
	(cd "$here/include/ps5platform" && sha256sum agc.h kernel.h videoout.h | sed 's/^/  /')
	echo "meson options: $(cat "$OPTIONS")"
	echo "cross files: sdk constant above + PS5_Vulkan tooling/radv/ps5-cross.ini"
	echo "archive sha256: $(sha256sum "$out/lib/libvulkan_radeon.ps5.a" | cut -d' ' -f1)"
} >"$out/PROVENANCE.txt"
[[ ${KEEP_BUILD:-0} == 1 ]] || rm -rf "$BUILD" "$sdk"   # both made again by a rebuild
cat "$out/PROVENANCE.txt"
