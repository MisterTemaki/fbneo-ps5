#!/usr/bin/env python3
"""Collect the GPU components' existing notices for the installer and update archive."""
# SPDX-License-Identifier: MIT
from pathlib import Path
import sys

output, *inputs = map(Path, sys.argv[1:])
parts = ["FBNeo PS5: notices for the GPU renderer and its dependencies.\n"]
for source in inputs:
    parts.append(f"\n{'=' * 72}\n{source.as_posix()}\n{'=' * 72}\n")
    parts.append(source.read_text(encoding="utf-8").rstrip() + "\n")
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text("".join(parts), encoding="utf-8")
print(f"{output}: {len(inputs)} notice files")
