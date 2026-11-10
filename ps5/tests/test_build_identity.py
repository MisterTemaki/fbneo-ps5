#!/usr/bin/env python3
"""Changing included instructions or platform glue must change the peer compatibility fingerprint."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(__file__).resolve().parents[1] / "tools/netplay_build_id.py"
with tempfile.TemporaryDirectory(prefix="fbneo-build-id-") as tmp:
    root = Path(tmp)
    script = root / "ps5/tools/netplay_build_id.py"
    script.parent.mkdir(parents=True)
    shutil.copyfile(source, script)
    changes = ["src/cpu/test.cpp", "src/cpu/instructions.inc", "src/cpu/assembly.asm", "src/cpu/inline.inl", "src/burn/renderer.ipp",
               "ps5/coreorbis/orbis-shims/ProsperoInput.cpp", "ps5/gpu-shaders/filter.slang", "ps5/tools/netplay_build_id.py"]
    required = ["ps5/Makefile", "ps5/gpu.mk", "ps5/gpu-shaders/presets.txt", "ps5/ps5-slang-gpu/VENDOR.txt",
                "ps5/core.mk", "ps5/core-base.txt", "ps5/tools/fbneo_sources.py", "makefile.burn_rules"]
    for rel in required + changes[:-1]:
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("initial\n")

    def identity():
        return subprocess.check_output([sys.executable, str(script)], text=True).strip()

    failed = 0
    for rel in changes:
        before = identity()
        p = root / rel
        p.write_bytes(p.read_bytes() + b"\n# changed\n")
        ok = identity() != before
        print(("PASS " if ok else "FAIL ") + rel)
        failed += not ok
    ok = identity() == identity()
    print(("PASS " if ok else "FAIL ") + "unchanged source remains reproducible")
    failed += not ok
    raise SystemExit(bool(failed))
