#!/usr/bin/env python3
"""The files each GPU shader preset pulls in, and the licence each one states.

    shader_licences.py <presets.txt> [--json out.json] [--markdown out.md]

For every preset of presets.txt (but the library's own, whose names start with "__"): the .slangp and every file it
#references, each pass's .slang and every file it #includes (recursively), and the look-up textures. Each file's
licence is read from its own text (its comments; a PNG has none): public domain, Unlicense, CC0, MIT, BSD, zlib,
GPL-2.0, GPL-3.0, GPL (no version), or "none stated". A preset is "embeddable" when every one of its files states
a licence among public domain, Unlicense, CC0, MIT, BSD and zlib; any GPL file, or any file with no licence of its
own, makes it "file" (shipped as separate files beside the program, in RetroStation's app folder, read and compiled
at run time; never in the program).

A .slangp states no licence: it is a few lines of settings (which passes, their scale and filter). It counts as
stating none, so a preset made of a .slangp is embeddable only through a descriptor of the library's own
(shaders/own/*.slangp, MIT), which names the same passes.

SPDX-License-Identifier: MIT
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import slang_compile  # noqa: E402  (parse_kv: the .slangp reader the build uses)

PERMISSIVE = {"public domain", "Unlicense", "CC0", "MIT", "BSD", "zlib"}


def licence_of(path):
    if path.lower().endswith((".png", ".jpg", ".jpeg", ".bmp", ".tga")):
        return "none stated"
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    # only the comments: a word in code (a variable named "mit") is no licence
    comments = " ".join(re.findall(r"//[^\n]*|/\*.*?\*/|^\s*#[^\n]*", text, flags=re.S | re.M))
    c = re.sub(r"\s+", " ", comments)
    found = []
    if re.search(r"GNU (Lesser )?General Public License|\bGPL\b|GPLv?[23]", c, re.I):
        # "or (at your option) any later version" / "or any later version": -or-later (the final M2 review)
        later = "-or-later" if re.search(r"or (\(at your option\) )?any later version", c, re.I) else ""
        if re.search(r"version 3|GPL-?v?3|GPLv3", c, re.I):
            found.append("GPL-3.0" + later)
        elif re.search(r"version 2|GPL-?v?2|GPLv2", c, re.I):
            found.append("GPL-2.0" + later)
        else:
            found.append("GPL")
    if re.search(r"\bUnlicen[cs]e\b", c, re.I):
        found.append("Unlicense")
    if re.search(r"\bCC0\b|Creative Commons Zero", c, re.I):
        found.append("CC0")
    if re.search(r"public[ -]domain", c, re.I) and "Unlicense" not in found and "CC0" not in found:
        found.append("public domain")
    if re.search(r"Permission is hereby granted, free of charge|\bMIT[- ]licen[cs]e|licen[cs]ed? under (the )?MIT|"
                 r"\bMIT\b\s*$|License:\s*MIT|SPDX-License-Identifier:\s*MIT", c, re.I | re.M):
        found.append("MIT")
    if re.search(r"Redistribution and use in source and binary forms", c, re.I):
        found.append("BSD")
    if re.search(r"This software is provided 'as-is', without any express or implied", c, re.I):
        found.append("zlib")
    return ", ".join(found) if found else "none stated"


def includes(path, seen):
    if path in seen:
        return
    seen.add(path)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r'\s*#include\s+"([^"]+)"', line)
            if m:
                includes(os.path.normpath(os.path.join(os.path.dirname(path), m.group(1))), seen)


def references(path, out):
    """the .slangp and every .slangp it #references"""
    if path in out:
        return
    out.append(path)
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r'\s*#reference\s+"?([^"\s]+)"?', line)
            if m:
                references(os.path.normpath(os.path.join(os.path.dirname(path), m.group(1))), out)


def closure(preset_path):
    files = []
    if preset_path.endswith(".slang"):
        seen = set()
        includes(preset_path, seen)
        return sorted(seen)
    refs = []
    references(preset_path, refs)
    files += refs
    kv = slang_compile.parse_kv(preset_path)
    n = int(kv.get("shaders", "0"))
    seen = set()
    for i in range(n):
        s = kv.get("shader%d" % i)
        if s:
            includes(s, seen)
    files += sorted(seen)
    for t in [t.strip() for t in kv.get("textures", "").split(";") if t.strip()]:
        p = kv.get(t)
        if p and os.path.exists(p):
            files.append(os.path.normpath(p))
    return list(dict.fromkeys(files))  # each file once, in the order found


def is_permissive(lic):
    parts = [p.strip() for p in lic.split(",")]
    return lic != "none stated" and not any(p.startswith("GPL") for p in parts) and all(p in PERMISSIVE for p in parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("presets")
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    ap.add_argument("--check", action="store_true",
                    help="fail (exit 1) if a preset the list builds in has a file that is not public domain, Unlicense, "
                         "CC0, MIT, BSD or zlib")
    a = ap.parse_args()
    base = os.path.dirname(os.path.abspath(a.presets))
    result = []
    for line in open(a.presets, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, spec = [x.strip() for x in line.split("=", 1)]
        if name.startswith("__"):
            continue
        # presets-rs.txt: "file:<path>" (separate files in the app folder) or a built-in one
        wanted = "file" if spec.startswith("file:") else "embed"
        spec = spec[len("file:"):] if wanted == "file" else spec
        path = os.path.normpath(os.path.join(base, spec.split("@", 1)[0].split("|", 1)[0]))
        files = []
        for f in closure(path):
            files.append({"path": os.path.relpath(f, base), "licence": licence_of(f), "bytes": os.path.getsize(f)})
        embeddable = all(is_permissive(f["licence"]) for f in files)
        result.append({"name": name, "preset": spec, "embeddable": embeddable, "wanted": wanted, "files": files})
    for p in result:
        bad = [f for f in p["files"] if not is_permissive(f["licence"])]
        print("%-26s %-10s %3d files, %7d bytes%s" % (p["name"], "embed" if p["embeddable"] else "file",
              len(p["files"]), sum(f["bytes"] for f in p["files"]),
              "" if not bad else "; not permissive: " + ", ".join("%s (%s)" % (f["path"], f["licence"]) for f in bad[:3]) +
              (" ..." if len(bad) > 3 else "")))
    if a.check:
        wrong = [p for p in result if p["wanted"] == "embed" and not p["embeddable"]]
        for p in wrong:
            print("shader_licences: %s is built in, but not every file of it is permissive" % p["name"], file=sys.stderr)
        if wrong:
            return 1
        print("shader_licences: every built-in preset's files are permissive (%d presets)" %
              sum(p["wanted"] == "embed" for p in result))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(result, f, indent=1)
    if a.markdown:
        with open(a.markdown, "w") as f:
            for p in result:
                f.write("### %s (%s)\n\n| file | bytes | licence |\n|---|---|---|\n" % (p["name"],
                        "built in" if p["wanted"] == "embed" else "separate files"))
                for x in p["files"]:
                    f.write("| `%s` | %d | %s |\n" % (x["path"], x["bytes"], x["licence"]))
                f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
