#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# Compares two RADV archives built from the same sources: every member, in order (members of the same name by
# position), byte for byte once its debug information is stripped (paths in it differ from build to build).
#   compare-archives.sh <old.a> <new.a> [work-dir]
set -eu
old=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
new=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
w=${3:-$(mktemp -d)}
mkdir -p "$w"
if ! llvm-ar t "$old" | cmp -s - "$(llvm-ar t "$new" >"$w/new.list" && echo "$w/new.list")"; then
	echo "the member lists differ"; exit 1
fi
same=0 diff=0
awk '{n[$0]++; print n[$0] " " $0}' "$w/new.list" >"$w/members"
while read -r k name; do
	for side in old new; do
		eval a=\$$side
		(cd "$w" && rm -f "$name" && llvm-ar xN "$k" "$a" "$name" && mv "$name" "$side.o")
		llvm-objcopy --strip-debug "$w/$side.o" "$w/$side.s.o"
	done
	if cmp -s "$w/old.s.o" "$w/new.s.o"; then same=$((same + 1)); else diff=$((diff + 1)); echo "differs: $name ($k)"; fi
done <"$w/members"
rm -f "$w/old.o" "$w/new.o" "$w/old.s.o" "$w/new.s.o"
echo "members identical without debug information: $same, different: $diff"
[ $diff = 0 ]
