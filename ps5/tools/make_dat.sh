#!/bin/sh
# FBNeo PS5: the ROM DAT of the arcade drivers the port compiles (`make dat` -> dat/fbneo-ps5-arcade.dat; the tests
# write it again and compare). The FBNeo commit and date in its header are the last commit of FBNeo's sources here
# (src/ and makefile.burn_rules, the core's build list), from git.
#
#   make_dat.sh <fbneo_headless> <out.dat>
#
# SPDX-License-Identifier: MIT
set -eu
headless=$1
out=$2
root=$(cd "$(dirname "$0")/../.." && pwd)
line=$(git -C "$root" log -1 --format='%H %cs' -- src makefile.burn_rules) || {
	echo "make_dat.sh: needs the git history (the FBNeo commit goes in the DAT's header)" >&2
	exit 1
}
[ -n "$line" ] || { echo "make_dat.sh: no FBNeo commit found" >&2; exit 1; }
commit=${line% *}
date=${line#* }
# the core's own scratch folder (fbneo_headless writes nothing there for a DAT)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ASAN_OPTIONS=detect_leaks=0 TMPDIR=$tmp "$headless" dat "$out" "$commit" "$date"
