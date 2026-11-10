#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
"""Compares the MIT stand-in headers (include/ps5platform/) with another set of headers of the same names, by
compiling, one item at a time, a file that includes the other header and then states the stand-in's item:

  - a macro:     _Static_assert((NAME) == (value), "")
  - a function:  the stand-in's declaration again (a conflicting type fails to compile)
  - a structure: the stand-in's body under another tag, then sizeof, and offsetof of every member, equal

  probe.py <other-include-dir> [--cc prospero-clang]

The other headers' text is never shown: each compile's diagnostics are discarded, and only "same" / "DIFFERENT"
per item is printed. Exit status 1 if any item differs or doesn't compile.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OURS = os.path.join(HERE, "include", "ps5platform")
# the members RADV's sources name (the others are only kept for the size)
USED = {"ps5_agc_submit_description": ("words", "word_count"), "ps5_video_out_buffer": ("data",)}


def items(path):
    text = open(path, encoding="utf-8").read()
    body = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    macros = re.findall(r"^#define\s+(PS5_[A-Z0-9_]+)\s+(.+)$", body, flags=re.M)
    funcs = [f.strip() + ";" for f in re.findall(r"^(?:int|int32_t|int64_t|void|size_t)\s+sce\w+\([^;]*\)", body, flags=re.M)]
    structs = re.findall(r"^struct\s+(\w+)\s*\{(.*?)\};", body, flags=re.M | re.S)
    return macros, funcs, structs


def compiles(cc, inc, header, extra):
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "probe.c")
        with open(src, "w") as f:
            f.write("#include <stddef.h>\n#include <stdint.h>\n#include <ps5platform/%s>\n%s\n" % (header, extra))
        r = subprocess.run([cc, "-std=gnu11", "-fsyntax-only", "-Werror", "-I", inc, src],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return r.returncode == 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("other_include")
    p.add_argument("--cc", default="/opt/ps5-payload-sdk/bin/prospero-clang")
    a = p.parse_args()
    bad = 0
    for header in sorted(os.listdir(OURS)):
        macros, funcs, structs = items(os.path.join(OURS, header))
        checks = []
        for name, value in macros:
            checks.append(("macro %s" % name, "_Static_assert((%s) == (%s), \"\");" % (name, value.strip())))
        for decl in funcs:
            checks.append(("function %s" % re.search(r"(sce\w+)\(", decl).group(1), decl))
        for tag, body in structs:
            members = re.findall(r"(\w+)(?:\[[^\]]*\])?\s*;", body)
            asserts = ["_Static_assert(sizeof(struct %s) == sizeof(struct mit_%s), \"\");" % (tag, tag)]
            asserts += ["_Static_assert(offsetof(struct %s, %s) == offsetof(struct mit_%s, %s), \"\");" %
                        (tag, m, tag, m) for m in members]
            # the other header's members may have other names: the size and the members the call sites use
            used = [m for m in members if m in USED.get(tag, ())]
            checks.append(("struct %s: size" % tag, "struct mit_%s {%s};\n%s" % (tag, body, asserts[0])))
            for m in used:
                checks.append(("struct %s: offset of %s" % (tag, m), "struct mit_%s {%s};\n%s" % (
                    tag, body, "_Static_assert(offsetof(struct %s, %s) == offsetof(struct mit_%s, %s), \"\");" %
                    (tag, m, tag, m))))
        if not compiles(a.cc, a.other_include, header, ""):
            print("%s: the other header doesn't compile on its own" % header)
            bad += 1
            continue
        for what, code in checks:
            ok = compiles(a.cc, a.other_include, header, code)
            print("%-9s %s: %s" % ("same" if ok else "DIFFERENT", header, what))
            bad += not ok
    print("probe: %s" % ("all the same" if bad == 0 else "%d items differ" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
