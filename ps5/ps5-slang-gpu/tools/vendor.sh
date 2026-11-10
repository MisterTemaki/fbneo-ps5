#!/bin/sh
# SPDX-License-Identifier: MIT
# Copies ps5-slang-gpu into a program, pinned (README.md, "Vendoring it").
#
#   tools/vendor.sh <dest> [commit]       e.g. tools/vendor.sh ../genplusgxPS5/ps5/ps5-slang-gpu
#
# <dest> is replaced by the files of <commit> (default HEAD; the work tree must be clean), unchanged, and VENDOR.txt
# beside them names the commit and every file with its sha256. The program's build runs tools/check_vendor.py on the
# copy (mk/slang-gpu.mk): a file changed, missing or added there fails it.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:?usage: tools/vendor.sh <dest> [commit]}
rev=${2:-HEAD}
commit=$(git -C "$here" rev-parse --verify "$rev^{commit}")
if [ "$rev" = HEAD ] && [ -n "$(git -C "$here" status --porcelain)" ]; then
	echo "vendor.sh: $here has changes not committed: commit them first (the copy is a commit's files)" >&2
	exit 1
fi
case $dest in /*) ;; *) dest=$(pwd)/$dest ;; esac
tmp=$dest.vendor-tmp
rm -rf "$tmp"
mkdir -p "$tmp"
git -C "$here" archive --format=tar "$commit" | tar -x -C "$tmp"
{
	echo "# ps5-slang-gpu, vendored by its tools/vendor.sh: the files of the commit below, unchanged."
	echo "# tools/check_vendor.py checks them (every program's build does); don't edit them here: change the library,"
	echo "# commit, and vendor it again."
	echo "commit $commit"
	(cd "$tmp" && find . -type f ! -name VENDOR.txt -printf '%P\n' | LC_ALL=C sort | while IFS= read -r f; do
		printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done)
} >"$tmp/VENDOR.txt"
rm -rf "$dest"
mv "$tmp" "$dest"
python3 -B "$dest/tools/check_vendor.py" "$dest" --against "$here"
