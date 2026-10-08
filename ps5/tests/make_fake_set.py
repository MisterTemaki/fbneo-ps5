#!/usr/bin/env python3
# FBNeo PS5 tests: a stand-in ROM set for a driver (no real ROMs are in the tests): every ROM the driver lists,
# filled with a pattern, under its name and with its CRC forged to match (so the loader finds it by CRC).
#
#   make_fake_set.py <fbneo_headless> <set> <out dir> [--fill zero|pattern] [--no-crc] [--drop N]
#                    [--put <rom name>=<file>]...   (that ROM's bytes from the file, zero-padded; its CRC still forged)
#
# The sets are written as the driver's zips ask (the set, its parent, its BIOS/board set).
# SPDX-License-Identifier: MIT
import os
import struct
import subprocess
import sys
import zipfile
import zlib


def forge_crc(data, crc):
    """data with its last 4 bytes changed so that zlib.crc32(result) == crc (the standard CRC-32 patch)."""
    if len(data) < 4:
        return data
    body = data[:-4]
    # CRC-32 reversed polynomial: work back from the target through 4 zero bytes
    poly = 0xEDB88320
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ poly if c & 1 else c >> 1
        table.append(c)
    inv = {}
    for i, t in enumerate(table):
        inv[t >> 24] = (t, i)
    cur = zlib.crc32(body) ^ 0xFFFFFFFF
    target = crc ^ 0xFFFFFFFF
    # find 4 bytes b so that crc(cur, b) == target: invert the table steps
    patch = [0, 0, 0, 0]
    t = target
    for k in range(3, -1, -1):
        e, i = inv[t >> 24]
        t = ((t ^ e) << 8) & 0xFFFFFFFF | i
        patch[k] = i
    # the low byte of t now has to come from cur: solve forwards
    c = cur
    out = bytearray()
    for k in range(4):
        b = (patch[k] ^ c) & 0xFF
        out.append(b)
        c = (c >> 8) ^ table[(c ^ b) & 0xFF]
    res = body + bytes(out)
    assert zlib.crc32(res) == crc, 'forge failed'
    return res


def main():
    tool, game, out = sys.argv[1], sys.argv[2], sys.argv[3]
    fill = 'pattern'
    crc_ok = True
    drop = -1
    a = sys.argv[4:]
    if '--fill' in a:
        fill = a[a.index('--fill') + 1]
    if '--no-crc' in a:
        crc_ok = False
    if '--drop' in a:
        drop = int(a[a.index('--drop') + 1])
    puts = {}
    for i, x in enumerate(a):
        if x == '--put':
            name, path = a[i + 1].split('=', 1)
            puts[name.lower()] = open(path, 'rb').read() if path else b''
    os.makedirs(out, exist_ok=True)
    zips = []
    sets_roms = {}
    # each zip of the set holds the ROMs its own driver lists
    lines = subprocess.run([tool, 'roms', game], capture_output=True, text=True, check=True).stdout.splitlines()
    zips = [l.split('\t')[1] for l in lines if l.startswith('zip\t')]
    boards = [l.split('\t')[1] for l in lines if l.startswith('board\t')]
    for z in zips:
        rl = subprocess.run([tool, 'roms', z], capture_output=True, text=True, check=True).stdout.splitlines()
        sets_roms[z] = [l.split('\t') for l in rl if not l.startswith(('zip\t', 'board\t'))]
    # a split set, as FBNeo's own lists make them: a ROM goes in the BIOS set when it is one of its ROMs, else in the
    # oldest parent that has it, else in the game's own zip
    order = boards + [z for z in reversed(zips) if z not in boards]
    owner = {}
    for z in order:
        for idx, name, length, crc, typ in sets_roms[z]:
            key = (name.lower(), crc)
            if name and key not in owner:
                owner[key] = z
    written = set()
    for z in zips:
        path = os.path.join(out, z + '.zip')
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as zf:
            for idx, name, length, crc, typ in sets_roms[z]:
                length, crc, typ = int(length), int(crc, 16), int(typ, 16)
                if not name or length == 0 or typ == 0 or (z, name) in written:
                    continue
                if owner.get((name.lower(), '%08x' % crc)) != z:
                    continue
                if z == game and int(idx) == drop:
                    continue
                if name.lower() in puts:
                    data = (puts[name.lower()] + bytes(length))[:length]
                elif fill == 'zero' or length > (1 << 20):
                    data = bytes(length)
                else:
                    data = bytes((i * 7 + 3) & 0xFF for i in range(length))
                if crc_ok and crc:
                    data = forge_crc(data, crc)
                zf.writestr(name, data)
                written.add((z, name))
    print(' '.join(zips))


main()
