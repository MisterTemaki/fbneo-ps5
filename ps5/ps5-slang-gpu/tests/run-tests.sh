#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# ps5-slang-gpu's host tests (make test): the library on the host's Vulkan driver (lavapipe), the stub, the tools.
# Exit 0 when every check passes.
set -u
cd "$(dirname "$0")/.."
W=build/tests
rm -rf "$W" && mkdir -p "$W"
pass=0; fail=0
ok() { echo "ok   $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
expect() { if eval "$1"; then ok "$2"; else bad "$2"; fi; }
# a test binary's own checks counted with ours
run() { # name command...
	local out=$W/$1.txt; shift
	"$@" >"$out" 2>"$out.log"; local rc=$?
	cat "$out" | grep -E '^(ok|FAIL) '
	pass=$((pass + $(grep -c '^ok ' "$out"))); fail=$((fail + $(grep -c '^FAIL ' "$out")))
	if [ $rc != 0 ] && ! grep -q '^FAIL ' "$out"; then bad "$1 exited $rc (see $out.log)"; fi
}
GPU="env ASAN_OPTIONS=detect_leaks=0 timeout 900 build/host/gpu-test"
APP0=$W/app0
make -s shaders-install DEST=$PWD/$APP0/shaders/slang >/dev/null
export GPU_TEST_SHIPPED=$PWD/$APP0/shaders/slang

echo "== the tools: tables, manifest, the shipped folder"
python3 -B tools/shader_manifest.py shaders/presets.txt $W/files-again.cpp >/dev/null
expect "cmp -s build/gen/slang_files.cpp $W/files-again.cpp" "the manifest's table made again: the same bytes"
python3 -B tools/slang_compile.py $W/presets-again.cpp --list shaders/presets.txt >/dev/null
expect "cmp -s build/gen/slang_presets.cpp $W/presets-again.cpp" "the built-in presets' tables, compiled again: the same bytes (every program gets the same SPIR-V)"
expect "! grep -q '#version\|void main\|#pragma' build/gen/slang_files.cpp && [ \$(grep -cE '^	\{\"[^\"]+\", [0-9]+u, \"[0-9a-f]{64}\"\},' build/gen/slang_files.cpp) = 255 ]" "the manifest: 255 files by name, size and sha256, no shader code"
expect "python3 -B tools/shader_licences.py shaders/presets.txt --check >/dev/null" "every built-in preset's files are permissive"
id=$(sha256sum <$APP0/shaders/slang/MANIFEST | cut -d' ' -f1)
expect "grep -q \"rs-shaders-manifest=$id\" build/gen/slang_files.cpp && [ $id = ff9768ad77c134fc391742745dc5770b9af1668f04bd6965188137cdd3b43e7b ]" "the manifest's id ${id:0:16}: the folder's MANIFEST, built in, and Genesis Plus GX PS5 rs1 95cf4b4's"
expect "(cd $APP0/shaders/slang && sha256sum --check --strict --quiet SHA256SUMS) && [ \$(find $APP0/shaders/slang -type f | wc -l) = 260 ] && grep -q 'not part of any program of Temakix RetroStation' $APP0/shaders/slang/LICENSES.md && grep -q 'Version 3, 29 June 2007' $APP0/shaders/slang/LICENSES.md" "the shipped folder: 255 files, LICENSES.md (not part of any program, the GPL texts), MANIFEST, SHA256SUMS, two licence files"
cp -a $APP0/shaders/slang $W/chk && printf x >>$W/chk/crt/crt-pi.slangp
expect "! python3 -B tools/shader_manifest.py shaders/presets.txt $W/x.cpp --check-folder $W/chk 2>/dev/null" "--check-folder refuses a changed file"
rm -rf $W/chk && cp -a $APP0/shaders/slang $W/chk && printf x >$W/chk/extra.txt
expect "! python3 -B tools/shader_manifest.py shaders/presets.txt $W/x.cpp --check-folder $W/chk 2>/dev/null" "... a file added"
rm -rf $W/chk && cp -a $APP0/shaders/slang $W/chk && ln -s /etc/passwd $W/chk/crt/link.slang
expect "! python3 -B tools/shader_manifest.py shaders/presets.txt $W/x.cpp --check-folder $W/chk 2>/dev/null" "... a symbolic link"
rm -rf $W/chk
printf 'int x;\n// "Scanline Intensity"\n' >$W/plain.bin; printf 'built-in preset(s) crt-guest-advanced ' >>$W/plain.bin
expect "python3 -B tools/check_no_shader_code.py shaders/presets.txt $W/plain.bin >/dev/null" "check_no_shader_code: a binary without GPL text passes"
cp $W/plain.bin $W/planted.bin && printf 'crt-royale: A full-featured CRT shader, with cheese.' >>$W/planted.bin
expect "! python3 -B tools/check_no_shader_code.py shaders/presets.txt $W/planted.bin 2>/dev/null" "... and finds a planted line of crt-royale"

echo "== the vendored copy's check"
if git rev-parse --verify -q HEAD >/dev/null && [ -z "$(git status --porcelain)" ]; then
	sh tools/vendor.sh $W/vendored >$W/vendor.txt 2>&1; rc=$?
	expect "[ $rc = 0 ] && grep -q 'unchanged' $W/vendor.txt && grep -q \"^commit \$(git rev-parse HEAD)\$\" $W/vendored/VENDOR.txt" "tools/vendor.sh: the commit's files and VENDOR.txt, checked against the repository"
	cp -a $W/vendored $W/v2 && printf x >>$W/v2/src/gpu.cpp
	expect "! python3 -B tools/check_vendor.py $W/v2 2>/dev/null" "check_vendor: a file changed fails it"
	rm -rf $W/v2 && cp -a $W/vendored $W/v2 && printf x >$W/v2/src/extra.cpp
	expect "! python3 -B tools/check_vendor.py $W/v2 2>/dev/null" "... a file added"
	rm -rf $W/v2 && cp -a $W/vendored $W/v2 && rm $W/v2/shaders/own/monoCRT.slangp
	expect "! python3 -B tools/check_vendor.py $W/v2 2>/dev/null" "... a file missing"
	rm -rf $W/v2 $W/vendored
else
	bad "the vendored copy's check needs a clean, committed work tree (git status)"
fi

echo "== the stub (a CPU-only build)"
run stub build/host/stub-test

echo "== SHA-256, Fit"
run unit build/host/gpu-test unit

echo "== every preset of the menu, compiled and drawing (lavapipe)"
run presets $GPU presets
expect "[ \$(grep -c '^ok   shader ' $W/presets.txt) = 27 ] && ! grep -qi 'could not\|failed' $W/presets.txt.log" "27 shaders (Off and the 26 presets) drew, nothing failed in the log"
expect "diff <(sed -n 's/^ok   shader [0-9]* \(.*\): [0-9]* pass.*/\1/p' $W/presets.txt | tail -n +2) <(grep -v '^#\|^__\|^\$' shaders/presets.txt | sed 's/ *=.*//')" "the menu's order and names are presets.txt's"

echo "== pixel formats, pitch, crop, rotation, the UI surface"
run formats $GPU formats
run ui-straight $GPU ui-straight

echo "== 1.1: video pictures (layouts, bit depths, matrices, HDR tone mapping, the UI over them, in place)"
run video $GPU video
run video-image-fail env PS5GPU_TEST_VIDEO_IMAGE_FAIL=1 $GPU video-image-fail
run video-keep $GPU video-keep
expect "grep -q 'video: ready' $W/video.txt.log && grep -q 'video self-test 1' $W/video.txt.log" "the video pipeline's self-test ran at Init (8-bit NV12 and 10-bit 4:4:4)"

echo "== damaged and missing files (the shipped folder is never written)"
listing() { (cd "$1" && find . -printf '%p %s %T@\n' | sort); }
D=$W/damaged
cp -a $APP0 $D
F=$D/shaders/slang/crt/shaders/guest/advanced/crt-guest-advanced.slang
python3 -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[100]^=1; open(p,'wb').write(b)" $F
listing $D >$W/before.txt
GPU_TEST_SHIPPED=$PWD/$D/shaders/slang $GPU damaged crt-guest-advanced >$W/d1.txt 2>$W/d1.log
expect "grep -q 'damaged at start: none' $W/d1.txt && grep -q 'crt-guest-advanced after it was picked: damaged, menu \"crt-guest-advanced: damaged, reinstall\"' $W/d1.txt && grep -q 'plain picture: yes' $W/d1.txt && grep -q 'crt-guest-advanced: damaged (crt/shaders/guest/advanced/crt-guest-advanced.slang is not the file the manifest names): reinstall Temakix RetroStation' $W/d1.log" "a changed file (same size): found when the preset is compiled (sha256), damaged, the plain picture"
listing $D >$W/after.txt
expect "cmp -s $W/before.txt $W/after.txt" "... nothing written, changed or removed there"
rm -f $D/shaders/slang/crt/shaders/crt-royale/src/bloom-functions.h
GPU_TEST_SHIPPED=$PWD/$D/shaders/slang $GPU damaged crt-royale >$W/d2.txt 2>$W/d2.log
expect "grep -q 'damaged at start: crt-guest-advanced,crt-royale\|damaged at start: crt-royale' $W/d2.txt && grep -q 'crt-royale after it was picked: damaged' $W/d2.txt && grep -q 'plain picture: yes' $W/d2.txt && grep -q '17 of 18 read from files in the app folder ready' $W/d2.log" "a missing file: crt-royale damaged from the start (sizes), 17 of 18 ready, the plain picture"
GPU_TEST_SHIPPED=$PWD/$W/no-such-folder $GPU damaged crt-lottes >$W/d3.txt 2>$W/d3.log
expect "[ \$(grep -o 'damaged at start: .*' $W/d3.txt | tr ',' '\n' | wc -l) = 18 ] && grep -q 'crt-lottes after it was picked: ready' $W/d3.txt && grep -q '0 of 18 read from files in the app folder ready' $W/d3.log" "no shipped folder: the 18 damaged, a built-in one still drawn"
rm -rf $D

echo "== the CPU fall-back, the start guard, the parameters file"
VK_ICD_FILENAMES=/nonexistent.json VK_DRIVER_FILES=/nonexistent.json $GPU init >$W/init.txt 2>$W/init.log
expect "grep -q 'init: cpu' $W/init.txt && grep -q '\[gpu\]' $W/init.log" "no Vulkan driver: Init is false (the program draws on the CPU), the log says why ($(grep '\[gpu\]' $W/init.log | head -1))"
$GPU init >$W/init2.txt 2>&1
expect "grep -q 'init: gpu' $W/init2.txt" "with lavapipe: Init is true"
run guard $GPU guard $PWD/$W/marker/gpu-starting
run params-set $GPU params $PWD/$W/params/shader-params.ini set
expect "grep -q \"^crt-lottes	\" $W/params/shader-params.ini" "the parameters file written (its folder made), keyed by the preset's name"
run params-get $GPU params $PWD/$W/params/shader-params.ini get
expect "[ ! -e $W/params/shader-params.ini ]" "... the parameters file removed once nothing is set"

echo
echo "ps5-slang-gpu host tests: $pass passed, $fail failed"
[ $fail = 0 ]
