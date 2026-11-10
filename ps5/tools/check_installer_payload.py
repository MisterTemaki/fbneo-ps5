#!/usr/bin/env python3
"""Verify the release ELF embeds the complete app, rather than delivering an intermediate PIE."""
# SPDX-License-Identifier: MIT
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib


def check(installer, app):
    data = installer.read_bytes()
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("expected a little-endian ELF64 installer")
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    segments = []
    for index in range(header[10]):
        segment = struct.unpack_from("<IIQQQQQQ", data, header[5] + index * header[9])
        if segment[0] == 1:
            segments.append(segment)
    names = subprocess.run(["nm", "--defined-only", str(installer)], check=True,
                           capture_output=True, text=True).stdout
    symbols = {}
    for line in names.splitlines():
        match = re.match(r"^([0-9a-fA-F]+)\s+\S\s+(fbneo_app_\w+)$", line)
        if match:
            symbols[match[2]] = int(match[1], 16)
    files = [
        ("eboot", "eboot.bin", True),
        ("param", "sce_sys/param.json", False),
        ("icon0", "sce_sys/icon0.png", False),
        ("pic0", "sce_sys/pic0.dds", True),
        ("pic0", "sce_sys/pic1.dds", True),
        ("libc", "sce_module/libc.prx", False),
        ("gpu_notices", "NOTICES-GPU.txt", False),
    ]
    for name, relative, packed in files:
        prefix = "fbneo_app_" + name
        if prefix + "_begin" not in symbols or prefix + "_end" not in symbols:
            raise ValueError(f"missing {prefix}: this ELF is not the complete installer; build 'dist'")
        begin, end = symbols[prefix + "_begin"], symbols[prefix + "_end"]
        blob = None
        for segment in segments:
            if segment[3] <= begin <= end <= segment[3] + segment[5]:
                offset = segment[2] + begin - segment[3]
                if offset + end - begin > len(data):
                    raise ValueError(f"truncated embedded {relative}")
                blob = data[offset:offset + end - begin]
                break
        if blob is None:
            raise ValueError(f"embedded {relative} is outside the ELF loadable segments")
        expected = (app / relative).read_bytes()
        if packed:
            if len(blob) < 12 or blob[:4] != b"FBZ1" or struct.unpack_from("<Q", blob, 4)[0] != len(expected):
                raise ValueError(f"invalid packed size for {relative}")
            blob = zlib.decompress(blob[12:])
        if blob != expected:
            raise ValueError(f"embedded {relative} differs from the packaged app")
        print(f"installer: {relative}: {len(expected)} bytes verified")
    print(f"installer: all {len(files)} app files verified")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: check_installer_payload.py <release.elf> <app-folder>")
    try:
        check(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError, struct.error, zlib.error, subprocess.CalledProcessError) as error:
        sys.exit(f"installer: FAILED: {error}")
