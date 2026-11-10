#!/usr/bin/env python3
"""The check that no GPL shader text is in a program (every RetroStation program with the GPU renderer).

    check_no_shader_code.py <presets-rs.txt> <binary>...

The presets that aren't built in ship as separate files beside the program (tools/shader_manifest.py --install); none
of their GPL text may be built into it, as source or as compiled tables. The markers come from the manifest (the final
M2 review): every `#pragma parameter` label of every GPL file of those presets (its own licence or its folder's,
shader_manifest.file_licence) -- the text a compiled table of it would hold --, but those the built-in presets (or the
library's own plain / ui shaders) also have, and a few header lines of crt-guest-advanced, crt-royale and the Mega Bezel
(source text). Each binary (the PIE, the signed eboot.bin) must hold none of them.

It can't pass by not looking: every header marker must be in its file; every preset with a GPL file must have at least
one marker of its own; and each binary must hold a string the program is known to have (a format string of
ps5-slang-gpu's src/slang_loader.cpp) and the name of a preset the manifest names, so a binary that can't be read as text (compressed,
encrypted) fails it.

SPDX-License-Identifier: MIT
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shader_licences  # noqa: E402
import shader_manifest  # noqa: E402

# (file of libretro/slang-shaders it comes from, the text): source text a compiled table wouldn't hold
HEADERS = [
    ("crt/shaders/guest/advanced/crt-guest-advanced.slang", "CRT - Guest - Advanced"),
    ("crt/shaders/guest/advanced/crt-guest-advanced.slang", "Copyright (C) 2018-2025 guest(r)"),
    ("crt/shaders/crt-royale/src/crt-royale-scanlines-vertical-interlacing.slang",
     "crt-royale: A full-featured CRT shader, with cheese."),
    ("crt/shaders/crt-royale/src/crt-royale-scanlines-vertical-interlacing.slang", "TroggleMonkey"),
    ("bezel/Mega_Bezel/shaders/base/common/params-0-screen-scale.inc", "HSM_MEGA_BEZEL_TITLE"),
]
# in every binary of this build: the program's own text, and a preset's name from the manifest (no code)
KNOWN = [b"built-in preset(s)", b"crt-guest-advanced"]
PARAM = re.compile(r'^\s*#pragma\s+parameter\s+\S+\s+"([^"]*)"', re.M)
MIN_LABEL = 12  # shorter labels ("Gamma", "Brightness") aren't distinctive


def labels(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    return {m.strip() for m in PARAM.findall(text)}


def distinctive(label):
    return len(label) >= MIN_LABEL and len(re.findall(r"[A-Za-z]", label)) >= 6


def markers(presets_txt):
    """[(preset, file, text)] and the GPL presets without a marker"""
    slang, _, files, presets, _ = shader_manifest.read_list(presets_txt)
    base = os.path.dirname(os.path.abspath(presets_txt))
    # the built-in presets' files, and the library's own shaders: their labels aren't markers
    own = set()
    for line in open(presets_txt, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, spec = [x.strip() for x in line.split("=", 1)]
        if spec.startswith("file:"):
            continue
        for f in shader_licences.closure(os.path.normpath(os.path.join(base, spec))):
            own |= labels(f)
    gpl = {p for p, _, _ in files if "GPL" in shader_manifest.file_licence(slang, p)[0]}
    out, uncovered, seen = [], [], set()
    for name, _, _, _, mine in presets:
        mine_gpl = [files[x][0] for x in mine if files[x][0] in gpl]
        found = 0
        for p in mine_gpl:
            for lab in sorted(labels(os.path.join(slang, p))):
                if distinctive(lab) and lab not in own:
                    found += 1
                    if lab not in seen:
                        seen.add(lab)
                        out.append((name, p, lab))
        if mine_gpl and not found:
            uncovered.append(name)
    return slang, out, uncovered


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    presets_txt, binaries = sys.argv[1], sys.argv[2:]
    slang, found, uncovered = markers(presets_txt)
    for rel, text in HEADERS:
        path = os.path.join(slang, rel)
        if not os.path.isfile(path) or text not in open(path, encoding="utf-8", errors="replace").read():
            print("check_no_shader_code: %s is not in %s: the check can't look for it" % (text, path), file=sys.stderr)
            return 1
    if uncovered:
        print("check_no_shader_code: no marker for %s: the check can't look for them" % ", ".join(uncovered),
              file=sys.stderr)
        return 1
    texts = [(rel, t) for rel, t in HEADERS] + [(p, lab) for _, p, lab in found]
    bad = 0
    for b in binaries:
        data = open(b, "rb").read()
        for k in KNOWN:
            if k not in data:
                print("check_no_shader_code: %s doesn't hold \"%s\": it can't be searched as text" % (b, k.decode()),
                      file=sys.stderr)
                return 1
        # the binary's printable runs (a table's strings, or source text), searched as one: fast for ~2000 markers
        runs = b"\n".join(re.findall(rb"[\x20-\x7e\t]{%d,}" % MIN_LABEL, data))
        for rel, text in texts:
            if text.encode() in runs or " ".join(text.split()).encode() in runs:
                print("check_no_shader_code: %s holds \"%s\" (%s): GPL shader text in the program" % (b, text, rel),
                      file=sys.stderr)
                bad += 1
    if bad:
        return 1
    print("check_no_shader_code: no GPL shader text in %s (%d markers: %d parameter labels of the GPL files of %d "
          "presets, %d header lines)" % (", ".join(os.path.basename(b) for b in binaries), len(texts), len(found),
                                         len({n for n, _, _ in found}), len(HEADERS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
