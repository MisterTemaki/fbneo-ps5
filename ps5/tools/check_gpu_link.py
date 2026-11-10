#!/usr/bin/env python3
"""Check that RADV's implemented Vulkan entry points survived its weak dispatch-table references."""
# SPDX-License-Identifier: MIT
import re
import subprocess
import sys

def exports(path):
    result = subprocess.run(["nm", "--defined-only", path], check=True, capture_output=True, text=True)
    return {m[1] for line in result.stdout.splitlines()
            if (m := re.search(r"\b[TtWw]\s+((?:radv|wsi)_[A-Z]\w*)$", line))}

binary, driver = sys.argv[1:]
required = exports(driver)
missing = required - exports(binary)
if not required or missing:
    sys.exit("GPU link: missing RADV entry points: " + ", ".join(sorted(missing)))
print(f"GPU link: all {len(required)} implemented RADV entry points linked")
