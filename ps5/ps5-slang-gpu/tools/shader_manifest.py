#!/usr/bin/env python3
"""The manifest of the shader presets that ship as separate files (ps5gpu/slang.h's kShipped*), as C++ tables, and
the files themselves.

    shader_manifest.py <presets-rs.txt> <out.cpp> [--json out.json] [--list out.txt] [--install DIR] [--table out.md]

presets-rs.txt lists the menu in order: "name = own/..." is built in (tools/slang_compile.py), "name =
file:slang/<path>[@param=value,...][|fullscreen]" is not. For each of the others the tables name every file it pulls
in (tools/shader_licences.py's closure: the .slangp and what it #references, each pass's .slang and what it #includes,
the look-up textures), by its path in libretro/slang-shaders (the path under shaders/slang/, which is that collection
at shaders/slang/REVISION), with its size and sha256. Names and hashes only: no shader code goes into the program.

--install DIR copies those files, unchanged (each with the licence written in it), to DIR/<path> -- RetroStation's
app folder's shaders/slang/, which every program reads -- and writes DIR/LICENSES.md beside them: which presets they are, where
they come from, the licence of each file (its own, or its folder's when it states none: FOLDER_LICENCES), and the
GPL's texts -- and DIR/SHA256SUMS, every file there with its sha256. DIR must not exist yet. The programs read them
there at run time and check every file against the sha256 of the tables before they compile a preset.

--list writes "<sha256> <bytes> <path>" lines (MANIFEST's text: its sha256 is the manifest's id, built into the
program as "rs-shaders-manifest=<id>", and --id-file writes it beside the program for RetroStation's make app).
--check-folder DIR checks an installed folder: MANIFEST is this list's, every file of SHA256SUMS is there and right,
nothing else is there (but SHA256SUMS), LICENSES.md is there.

SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shader_licences  # noqa: E402

UPSTREAM = "https://github.com/libretro/slang-shaders"
# licence files of the collection that sit beside files shipped: shipped with them, as they are
EXTRA_LICENCE_FILES = ["crt/shaders/crt-royale/LICENSE.TXT", "bezel/Mega_Bezel/README.md"]
# folders whose licence is stated for the whole folder, not in each file (the final M2 review): a file there whose own
# text names none is under the folder's. (prefix, licence, where the folder says so, its words)
FOLDER_LICENCES = [
    ("bezel/Mega_Bezel/", "GPL-3.0", "bezel/Mega_Bezel/README.md",
     "The base Mega_Bezel package and included components licensing are GPLv3"),
    ("crt/shaders/crt-royale/", "GPL-2.0-or-later", "crt/shaders/crt-royale/LICENSE.TXT",
     "the GNU General Public License, version 2, in full; crt-royale's headers say \"version 2 of the License, or any "
     "later version\""),
]
# files whose header is a notice but no licence (the final M2 review: Mega Bezel's FXAA pass, NVIDIA's 2011 notice). The
# folder's licence isn't given to them (review round 7): they are said as what they carry
NOTICE_ONLY = {
    "bezel/Mega_Bezel/shaders/fxaa/fxaa.slang":
        ("NVIDIA's notice only", "NVIDIA's notice only (no licence grant); shipped unchanged as part of the Mega Bezel "
         "package"),
}


def file_licence(slang, path):
    """(short, long): the licence a shipped file states, or its folder's when it states none; a notice-only header said
    as such. The short form is the summaries', the long one LICENSES.md's per file."""
    stated = shader_licences.licence_of(os.path.join(slang, path))
    note = NOTICE_ONLY.get(path)
    if note:
        return note
    short, long_ = stated, stated
    if stated == "none stated":
        for prefix, lic, where, _ in FOLDER_LICENCES:
            if path.startswith(prefix):
                short, long_ = lic + " (its folder)", "%s, its folder's (`%s`); none in the file" % (lic, where)
                break
    return short, long_


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def read_list(presets_txt):
    base = os.path.dirname(os.path.abspath(presets_txt))
    slang = os.path.join(base, "slang")
    revision = open(os.path.join(slang, "REVISION")).read().split()[0]
    if len(revision) != 40 or any(c not in "0123456789abcdef" for c in revision):
        raise SystemExit("shader_manifest: %s/REVISION is not a commit" % slang)
    files = []       # (path, bytes, sha256)
    index = {}
    presets = []     # (name, preset path, overrides, fullscreen, [file indices])
    order = []       # menu order: ("own", k) or ("file", d)
    own = 0
    for line in open(presets_txt, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, spec = [x.strip() for x in line.split("=", 1)]
        if name.startswith("__"):
            continue
        if not spec.startswith("file:"):
            order.append(("own", own))
            own += 1
            continue
        spec = spec[len("file:"):]
        fullscreen = False
        if "|" in spec:
            spec, flags = spec.split("|", 1)
            fullscreen = "fullscreen" in flags
        overrides = ""
        if "@" in spec:
            spec, overrides = spec.split("@", 1)
        path = os.path.normpath(os.path.join(base, spec))
        rel = os.path.relpath(path, slang)
        if rel.startswith(".."):
            raise SystemExit("shader_manifest: %s is not under shaders/slang" % spec)
        mine = []
        for f in shader_licences.closure(path):
            r = os.path.relpath(f, slang)
            if r.startswith("..") or not os.path.isfile(f) or os.path.islink(f):
                raise SystemExit("shader_manifest: %s pulls in %s, not a file of shaders/slang" % (name, f))
            if r not in index:
                data = open(f, "rb").read()
                index[r] = len(files)
                files.append((r, len(data), hashlib.sha256(data).hexdigest()))
            if index[r] not in mine:
                mine.append(index[r])
        presets.append((name, rel, overrides, fullscreen, mine))
        order.append(("file", len(presets) - 1))
    return slang, revision, files, presets, order


def list_text(files):
    """the manifest as "<sha256> <bytes> <path>" lines: MANIFEST in the app folder; its sha256 is the manifest's id"""
    return "".join("%s %d %s\n" % (h, n, p) for p, n, h in files)


def manifest_id(files):
    return hashlib.sha256(list_text(files).encode()).hexdigest()


ID_PREFIX = "rs-shaders-manifest="


def write_tables(out_path, list_name, revision, files, presets, order):
    out = ["// Generated by ps5-slang-gpu/tools/shader_manifest.py from %s: the shader presets that ship as separate files" %
           list_name,
           "// in RetroStation's app folder (libretro/slang-shaders at %s): their files' names, sizes and sha256, no" %
           revision, "// shader code. Do not edit.", "#include \"ps5gpu/slang.h\"", "", "namespace ps5slang", "{",
           "const char* const kShippedRevision = %s;" % c_str(revision),
           "// the manifest's id: RetroStation's make app finds it in the eboot and checks the folder it copies against it",
           "const char* const kShippedManifestId = %s;" % c_str(ID_PREFIX + manifest_id(files)),
           "const ShippedFile kShippedFiles[] = {"]
    for p, n, h in files:
        out.append("\t{%s, %du, %s}," % (c_str(p), n, c_str(h)))
    out.append("};")
    out.append("const int kShippedFileCount = %d;" % len(files))
    for i, (_, _, _, _, mine) in enumerate(presets):
        out.append("static const uint16_t shipped_files%d[] = {%s};" % (i, ", ".join(str(x) for x in mine)))
    out.append("const ShippedPreset kShippedPresets[] = {")
    for i, (name, rel, ov, fs, mine) in enumerate(presets):
        out.append("\t{%s, %s, %s, %s, shipped_files%d, %d, %du}," % (c_str(name), c_str(rel), c_str(ov),
                   "true" if fs else "false", i, len(mine), sum(files[x][1] for x in mine)))
    out.append("};")
    out.append("const int kShippedPresetCount = %d;" % len(presets))
    out.append("// the menu after the plain picture: >= 0 a built-in preset (kBuiltinPresets[2 + n]), < 0 one from the "
               "app folder (kShippedPresets[-1 - n])")
    out.append("const int16_t kMenuOrder[] = {%s};" % ", ".join(str(k) if kind == "own" else str(-1 - k)
                                                                   for kind, k in order))
    out.append("const int kMenuOrderCount = %d;" % len(order))
    out.append("} // namespace ps5slang")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path + ".part", "w") as f:
        f.write("\n".join(out) + "\n")
    os.replace(out_path + ".part", out_path)


def summary(lics):
    counts = {}
    for lic in lics:
        counts[lic] = counts.get(lic, 0) + 1
    return ", ".join("%s (%d)" % (k, counts[k]) for k in sorted(counts, key=lambda k: (k == "none stated", k)))


def licences_md(slang, revision, files, presets, builtin_count=8):
    both = {p: file_licence(slang, p) for p, _, _ in files}
    lic = {p: b[0] for p, b in both.items()}
    gpl = lambda names: any("GPL" in lic[files[x][0]] for x in names)  # noqa: E731
    total = sum(n for _, n, _ in files)
    o = []
    o.append("# The GPU shader presets in this folder, and their licences\n")
    o.append("These files are RetroArch slang shader presets from libretro's collection, **libretro/slang-shaders** "
             "(%s), at revision `%s`, unchanged: each keeps the licence written at its top. They are %d files, "
             "%d KB, for the %d presets below.\n" % (UPSTREAM, revision, len(files), (total + 1023) // 1024,
                                                     len(presets)))
    o.append("They are **not part of any program of Temakix RetroStation** (Genesis Plus GX PS5, Snes9x PS5, FBNeo "
             "PS5 and the others). They ship once, as separate files, in Temakix RetroStation's app folder "
             "(`shaders/slang`), and each program that draws with the GPU reads and compiles them when it runs (with "
             "glslang, on the console), as it does the presets a player puts in its own shaders folder. The programs "
             "have only their names, sizes and SHA-256 (to check each file before it is used), no shader code of "
             "theirs; their own code, and the %d presets built into them (public domain, Unlicense, CC0 and MIT), are "
             "unchanged.\n" % builtin_count)
    o.append("These files are their own source code (GLSL, and the textures as PNG images); the collection at that "
             "revision: %s/tree/%s.\n" % (UPSTREAM, revision))
    o.append("## The presets\n")
    o.append("| Preset | GPL | Files | KB | Licences of its files |\n|---|---|---|---|---|")
    for name, rel, _, _, mine in presets:
        o.append("| %s (`%s`) | %s | %d | %d | %s |" % (name, rel, "yes" if gpl(mine) else "no", len(mine),
                 (sum(files[x][1] for x in mine) + 1023) // 1024, summary(lic[files[x][0]] for x in mine)))
    o.append("")
    o.append("\"none stated\" is a file with no licence text of its own (a `.slangp` of settings, a texture, a helper "
             "header). It is shipped as it is in the collection, which states no licence for it, with the preset that "
             "uses it. \"GPL\" with no version is a file that names the GNU General Public License without a version: "
             "its terms let you choose any version (both texts are below). \"(its folder)\": the file states no licence, and "
             "its folder states one for every file in it:\n")
    for prefix, lic, where, words in FOLDER_LICENCES:
        o.append("- `%s**`: %s, as `%s` says (%s); that file is here too." % (prefix, lic, where, words))
    for path, (short, long_) in NOTICE_ONLY.items():
        o.append("- `%s`: %s. Its header is \"Copyright (c) 2011 NVIDIA Corporation. All rights reserved.\" and a "
                 "warranty disclaimer; the folder's licence isn't given to it." % (path, long_))
    o.append("")
    o.append("## Every file\n")
    o.append("| File | Bytes | SHA-256 | Licence |\n|---|---|---|---|")
    for p, n, h in files:
        o.append("| `%s` | %d | `%s` | %s |" % (p, n, h, both[p][1]))
    o.append("")
    o.append("Also here, as they are in the collection: %s.\n" % ", ".join("`%s`" % x for x in EXTRA_LICENCE_FILES))
    for title, path in (("GNU General Public License, version 2", "GPL-2.0.txt"),
                        ("GNU General Public License, version 3", "GPL-3.0.txt")):
        o.append("## %s\n" % title)
        o.append("```")
        o.append(open(os.path.join(os.path.dirname(slang), "licences", path), encoding="utf-8").read().rstrip("\n"))
        o.append("```\n")
    return "\n".join(o)


def install(dest, slang, revision, files, presets, id_file=None, builtin_count=8):
    if os.path.lexists(dest):
        raise SystemExit("shader_manifest: %s is there already (the build removes it first)" % dest)
    for p, n, h in files:
        src, dst = os.path.join(slang, p), os.path.join(dest, p)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        data = open(dst, "rb").read()
        if len(data) != n or hashlib.sha256(data).hexdigest() != h:
            raise SystemExit("shader_manifest: %s changed while it was copied" % p)
    for p in EXTRA_LICENCE_FILES:
        os.makedirs(os.path.dirname(os.path.join(dest, p)), exist_ok=True)
        shutil.copyfile(os.path.join(slang, p), os.path.join(dest, p))
    with open(os.path.join(dest, "LICENSES.md"), "w", encoding="utf-8") as f:
        f.write(licences_md(slang, revision, files, presets, builtin_count))
    with open(os.path.join(dest, "MANIFEST"), "w") as f:
        f.write(list_text(files))
    # every file here with its sha256 (sha256sum's format, paths from this folder): RetroStation's make app checks its
    # copy against it
    sums = []
    for top, _, names in os.walk(dest):
        for n in names:
            full = os.path.join(top, n)
            sums.append((os.path.relpath(full, dest), hashlib.sha256(open(full, "rb").read()).hexdigest()))
    with open(os.path.join(dest, "SHA256SUMS"), "w") as f:
        for rel, h in sorted(sums):
            f.write("%s  %s\n" % (h, rel))
    # beside the program (its folder's shaders-manifest.txt): the id its eboot carries, which RetroStation's make app
    # checks the shaders against
    if id_file:
        with open(id_file, "w") as f:
            f.write(manifest_id(files) + "\n")


def check_folder(dest, files):
    """an installed folder against this list: [] or what is wrong"""
    bad = []
    if not os.path.isfile(os.path.join(dest, "LICENSES.md")):
        bad.append("LICENSES.md is missing")
    try:
        if open(os.path.join(dest, "MANIFEST")).read() != list_text(files):
            bad.append("MANIFEST is not this list's")
    except OSError:
        bad.append("MANIFEST is missing")
    listed = {}
    try:
        for line in open(os.path.join(dest, "SHA256SUMS")):
            h, rel = line.rstrip("\n").split("  ", 1)
            listed[rel] = h
    except (OSError, ValueError):
        bad.append("SHA256SUMS is missing or unreadable")
    there = set()
    for top, dirs, names in os.walk(dest):
        for n in dirs + names:
            full = os.path.join(top, n)
            if os.path.islink(full) or not (os.path.isfile(full) or os.path.isdir(full)):
                bad.append("%s is not a file or a folder" % os.path.relpath(full, dest))
        for n in names:
            there.add(os.path.relpath(os.path.join(top, n), dest))
    there.discard("SHA256SUMS")
    for rel in sorted(there - set(listed)):
        bad.append("%s is not in SHA256SUMS" % rel)
    for rel, h in sorted(listed.items()):
        full = os.path.join(dest, rel)
        if not os.path.isfile(full):
            bad.append("%s is missing" % rel)
        elif hashlib.sha256(open(full, "rb").read()).hexdigest() != h:
            bad.append("%s is not the file SHA256SUMS names" % rel)
    return bad


def third_party_table(slang, files, presets):
    """the table of THIRD_PARTY_SHADERS.md's "Separate files" section"""
    lic = {p: file_licence(slang, p)[0] for p, _, _ in files}
    o = ["| Preset | Files | KB | Licences of its files |", "|---|---|---|---|"]
    for name, _, _, _, mine in presets:
        o.append("| %s | %d | %d | %s |" % (name, len(mine), (sum(files[x][1] for x in mine) + 1023) // 1024,
                                           summary(lic[files[x][0]] for x in mine)))
    return "\n".join(o) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("presets")
    ap.add_argument("out")
    ap.add_argument("--json")
    ap.add_argument("--list")
    ap.add_argument("--install")
    ap.add_argument("--table", help="write THIRD_PARTY_SHADERS.md's table of these presets here")
    ap.add_argument("--id-file", help="write the manifest's id here (beside the program; with --install or without)")
    ap.add_argument("--check-folder", help="check an installed folder against the list (exit 1 if it isn't right)")
    a = ap.parse_args()
    slang, revision, files, presets, order = read_list(a.presets)
    write_tables(a.out, os.path.basename(a.presets), revision, files, presets, order)
    total = sum(n for _, n, _ in files)
    print("%s: %d presets from files, %d files, %d KB, revision %s" % (a.out, len(presets), len(files),
          (total + 1023) // 1024, revision[:12]))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"revision": revision, "files": [{"path": p, "bytes": n, "sha256": h} for p, n, h in files],
                       "presets": [{"name": n, "preset": r, "files": [files[x][0] for x in m]}
                                   for n, r, _, _, m in presets]}, f, indent=1)
    if a.list:
        with open(a.list, "w") as f:
            f.write(list_text(files))
    if a.table:
        with open(a.table, "w") as f:
            f.write(third_party_table(slang, files, presets))
    if a.install:
        install(a.install, slang, revision, files, presets, a.id_file, sum(k == "own" for k, _ in order))
        print("%s: %d files and LICENSES.md" % (a.install, len(files)))
    if a.id_file and not a.install:
        with open(a.id_file, "w") as f:
            f.write(manifest_id(files) + "\n")
    if a.check_folder:
        bad = check_folder(a.check_folder, files)
        for b in bad:
            print("shader_manifest: %s: %s" % (a.check_folder, b), file=sys.stderr)
        if bad:
            return 1
        print("%s: the manifest %s, every file right" % (a.check_folder, manifest_id(files)[:16]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
