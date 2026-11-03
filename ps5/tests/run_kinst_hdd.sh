#!/bin/sh
# FBNeo PS5 2.2: makes the synthetic images (tests/make_chd.py) and runs Killer Instinct's hard disk test.
#
#   run_kinst_hdd.sh <test-kinst-hdd> <work dir>
#
# SPDX-License-Identifier: MIT
bin=$1
dir=$2
rm -rf "$dir"
mkdir -p "$dir"
here=$(dirname "$0")
python3 "$here/make_chd.py" "$dir/A.chd" --seed 1 >/dev/null &&
	python3 "$here/make_chd.py" "$dir/B.chd" --seed 2 --zlib >/dev/null &&
	python3 "$here/make_chd.py" "$dir/Z.chd" --seed 3 --zero-sha1 >/dev/null &&
	python3 "$here/make_chd.py" "$dir/R.img" --seed 4 --raw >/dev/null || { echo "  FAIL: make_chd.py"; exit 1; }
ASAN_OPTIONS=detect_leaks=1 "$bin" "$dir" 2>"$dir/log.txt"
