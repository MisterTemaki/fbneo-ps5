#!/bin/sh
# FBNeo PS5: the ROM DAT of the arcade drivers the port compiles (`make dat` -> dat/fbneo-ps5-arcade.dat; the tests
# write it again and compare). Its header names the FBNeo commit the core is based on and the last upstream commit taken
# since (ps5/core-base.txt), with their dates; the history must have them.
#
#   make_dat.sh <fbneo_headless> <out.dat>
#
# SPDX-License-Identifier: MIT
set -eu
headless=$1
out=$2
root=$(cd "$(dirname "$0")/../.." && pwd)
# the base commit and the upstream commits taken since (core-base.txt; 2.2: the Killer Instinct ones)
base=$(sed -n 's/^base \([0-9a-f]*\) \([0-9-]*\)$/\1 \2/p' "$root/ps5/core-base.txt")
last=$(sed -n 's/^pick \([0-9a-f]*\) \([0-9-]*\) .*/\1 \2/p' "$root/ps5/core-base.txt" | tail -1)
[ -n "$base" ] || { echo "make_dat.sh: no base commit in ps5/core-base.txt" >&2; exit 1; }
commit=${base% *}
date=${base#* }
# the history must have them: the base itself, and a commit with the last pick's subject after it
git -C "$root" cat-file -e "$commit^{commit}" 2>/dev/null || { echo "make_dat.sh: needs the git history ($commit)" >&2; exit 1; }
picks=""
if [ -n "$last" ]; then
	subject=$(sed -n 's/^pick [0-9a-f]* [0-9-]* //p' "$root/ps5/core-base.txt" | tail -1)
	git -C "$root" log --format=%s "$commit..HEAD" -- src makefile.burn_rules | grep -qxF "$subject" ||
		{ echo "make_dat.sh: the last commit core-base.txt names ($subject) isn't in the history" >&2; exit 1; }
	picks="${last% *} ${last#* }"
fi
# the core's own scratch folder (fbneo_headless writes nothing there for a DAT)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ASAN_OPTIONS=detect_leaks=0 TMPDIR=$tmp "$headless" dat "$out" "$commit" "$date" $picks
