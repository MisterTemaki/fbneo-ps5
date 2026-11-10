#!/usr/bin/env python3
"""Fingerprint the emulation/frontend sources; netplay peers must run matching builds."""
import hashlib
from pathlib import Path

root = Path(__file__).resolve().parents[2]
files = []
for directory in (root / "src", root / "ps5/frontend", root / "ps5/core", root / "ps5/coreorbis", root / "ps5/gpu-shaders",
                  root / "ps5/ps5-slang-gpu/src", root / "ps5/ps5-slang-gpu/include", root / "ps5/gpu-platform/src",
                  root / "ps5/gpu-platform/third_party"):
    files.extend(p for p in directory.rglob("*") if p.is_file() and p.suffix.lower() in {".c", ".cpp", ".h", ".hpp", ".s", ".asm", ".inc", ".inl", ".ipp", ".slang", ".slangp"})
files.extend(root / p for p in ("ps5/Makefile", "ps5/gpu.mk", "ps5/gpu-shaders/presets.txt", "ps5/ps5-slang-gpu/VENDOR.txt",
                              "ps5/core.mk", "ps5/core-base.txt", "ps5/tools/fbneo_sources.py", "ps5/tools/netplay_build_id.py", "makefile.burn_rules"))
digest = hashlib.sha256()
for path in sorted(set(files), key=lambda p: p.relative_to(root).as_posix()):
    name = path.relative_to(root).as_posix().encode()
    data = path.read_bytes()
    digest.update(len(name).to_bytes(4, "big") + name + len(data).to_bytes(8, "big") + data)
print(digest.hexdigest())
