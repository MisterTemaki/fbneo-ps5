#!/usr/bin/env python3
"""A program's copy of ps5-slang-gpu against its VENDOR.txt (tools/vendor.sh wrote both).

    check_vendor.py <copy> [--against <ps5-slang-gpu checkout>]

VENDOR.txt names the library's commit and every file of it ("<sha256>  <path>", paths from the copy). The copy must hold
exactly those files, each one unchanged: a file changed, missing or added fails it (exit 1), so a program builds only
from a pinned, untouched copy. --against also checks that the checkout's commit has those very files (git archive).

SPDX-License-Identifier: MIT
"""
import hashlib
import io
import os
import re
import subprocess
import sys
import tarfile


def read_vendor(copy):
    path = os.path.join(copy, "VENDOR.txt")
    commit, files = None, {}
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        m = re.match(r"commit ([0-9a-f]{40})$", line)
        if m:
            commit = m.group(1)
            continue
        m = re.match(r"([0-9a-f]{64})  (.+)$", line)
        if not m:
            raise SystemExit("check_vendor: %s: a line that is neither the commit nor a file: %r" % (path, line))
        files[m.group(2)] = m.group(1)
    if not commit or not files:
        raise SystemExit("check_vendor: %s names no commit or no file" % path)
    return commit, files


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"):
        print(__doc__, file=sys.stderr)
        return 2
    copy = args[0]
    against = args[args.index("--against") + 1] if "--against" in args else None
    commit, files = read_vendor(copy)
    bad = []
    there = set()
    for top, dirs, names in os.walk(copy):
        for n in dirs + names:
            full = os.path.join(top, n)
            if os.path.islink(full):
                bad.append("%s is a symbolic link" % os.path.relpath(full, copy))
        for n in names:
            there.add(os.path.relpath(os.path.join(top, n), copy))
    there.discard("VENDOR.txt")
    for rel in sorted(there - set(files)):
        bad.append("%s is not in VENDOR.txt (added)" % rel)
    for rel, h in sorted(files.items()):
        full = os.path.join(copy, rel)
        if not os.path.isfile(full):
            bad.append("%s is missing" % rel)
        elif hashlib.sha256(open(full, "rb").read()).hexdigest() != h:
            bad.append("%s is changed" % rel)
    if against:
        data = subprocess.run(["git", "-C", against, "archive", "--format=tar", commit], capture_output=True, check=True).stdout
        theirs = {}
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            for m in t.getmembers():
                if m.isfile():
                    theirs[m.name] = hashlib.sha256(t.extractfile(m).read()).hexdigest()
        if theirs != files:
            diff = sorted(set(theirs.items()) ^ set(files.items()))
            bad.append("VENDOR.txt is not commit %s of %s (%d differences, first %s)" % (commit[:12], against, len(diff),
                                                                                     diff[0][0]))
    for b in bad[:20]:
        print("check_vendor: %s: %s" % (copy, b), file=sys.stderr)
    if bad:
        print("check_vendor: %s is not ps5-slang-gpu %s as vendored (%d problems): run tools/vendor.sh again" %
              (copy, commit[:12], len(bad)), file=sys.stderr)
        return 1
    print("check_vendor: %s is ps5-slang-gpu %s, %d files unchanged" % (copy, commit[:12], len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
