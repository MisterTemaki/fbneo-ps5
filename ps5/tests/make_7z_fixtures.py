#!/usr/bin/env python3
"""Makes the .7z archives the 7z tests use (tests/7z/, committed: the tests need neither py7zr nor a network), as NES
FCEUmm PS5 1.1's script does for its reader. Needs py7zr (pip install py7zr) and the host build's fbneo_headless (the
stand-in sets come from tests/make_fake_set.py).

  make_7z_fixtures.py <fbneo_headless> <out-dir>

The reader (tests/test_7z.cpp), on rom.bin (16 KiB of seeded random bytes):
  lzma.7z        rom.bin, LZMA                         lzma2.7z    the same, LZMA2
  ppmd.7z        the same, PPMd                        bcj.7z      the same, x86 BCJ + LZMA2
  delta.7z       the same, Delta + LZMA2
  solid.7z       readme.txt, roms/a.bin, b.bin in one LZMA2 block (a name with its folder)
  empty.7z       no files at all
  bzip2.7z       rom.bin, BZip2: a method the decoder doesn't have
  aes.7z         rom.bin, AES-256 (password "fbneo"), names readable
  aeshdr.7z      the same with the names encrypted too
  ppmdbig.7z     rom.bin, PPMd with a 1 GiB model: over the decoder's budget (512 + 72 MiB)
  many.7z        70,000 empty files, then rom.bin: more files than an archive may list (65,536)
  bigblock.7z    rom.bin then 513 MiB of zeros in one solid block: over the 512 MiB block limit
  truncated.7z   lzma2.7z cut in half                  corrupt.7z  lzma2.7z with its packed data changed (CRC error)
The app (tests/run-tests.sh), stand-in sets as FBNeo names them:
  pacman.7z      the Pac-Man set with the test program (tests/data), one LZMA2 block: a .7z set that runs
  ponpoko.7z     the Ponpoko set, AES-256 (password "fbneo"): refused, "encrypted"
  bstarsh.7z     Baseball Stars Professional (NGH-002), a Neo Geo clone (its parent bstars and the BIOS neogeo are
                 zips the test makes)
  puckmanbb.7z   the Puck Man set plus 513 MiB of zeros in one block: listed (its directory is complete), refused
                 when started ("too big")
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile
import zipfile

import py7zr

HERE = os.path.dirname(os.path.abspath(__file__))

# py7zr writes PPMd's properties as 7 bytes (order, memory, two zero bytes); 7-Zip writes 5, and the LZMA SDK's C decoder
# takes only 5. The stream itself is the same PPMd (variant H, 7z's range coder): write the properties as 7-Zip does.
_ppmd_props = py7zr.compressor.PpmdCompressor.encode_filter_properties.__func__


def _ppmd_props_7zip(cls, f):
    return _ppmd_props(cls, f)[:5]


py7zr.compressor.PpmdCompressor.encode_filter_properties = classmethod(_ppmd_props_7zip)
_ppmd_decode = py7zr.compressor.PpmdCompressor._decode_property


def _ppmd_decode_7zip(self, properties):
    return _ppmd_decode(self, properties + b"\0\0" if len(properties) == 5 else properties)


py7zr.compressor.PpmdCompressor._decode_property = _ppmd_decode_7zip


def make(out, files, filters=None, password=None, header_encryption=False):
    if os.path.exists(out):
        os.remove(out)
    with py7zr.SevenZipFile(out, "w", filters=filters, password=password, header_encryption=header_encryption) as z:
        for name, data in files:
            z.writestr(data, name)


def fake_set(headless, name, extra=()):
    """The stand-in set's zips (make_fake_set.py), as {file name: [(member, bytes)...]}."""
    d = tempfile.mkdtemp()
    try:
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        subprocess.run([sys.executable, os.path.join(HERE, "make_fake_set.py"), headless, name, d] + list(extra), check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env, cwd=os.path.join(HERE, ".."))
        sets = {}
        for f in sorted(os.listdir(d)):
            with zipfile.ZipFile(os.path.join(d, f)) as z:
                sets[f] = [(i.filename, z.read(i.filename)) for i in z.infolist()]
        return sets
    finally:
        shutil.rmtree(d)


def main():
    headless, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    rng = random.Random(7)
    rom = bytes(rng.getrandbits(8) for _ in range(16 * 1024))
    readme = b"A test archive for FBNeo PS5.\n" * 4
    lz2 = [{"id": py7zr.FILTER_LZMA2, "preset": 9}]
    one = [("rom.bin", rom)]
    aes = [{"id": py7zr.FILTER_LZMA2}, {"id": py7zr.FILTER_CRYPTO_AES256_SHA256}]
    make(os.path.join(out, "lzma.7z"), one, [{"id": py7zr.FILTER_LZMA}])
    make(os.path.join(out, "lzma2.7z"), one, lz2)
    make(os.path.join(out, "ppmd.7z"), one, [{"id": py7zr.FILTER_PPMD, "order": 6, "mem": 24}])
    make(os.path.join(out, "bcj.7z"), one, [{"id": py7zr.FILTER_X86}, {"id": py7zr.FILTER_LZMA2}])
    make(os.path.join(out, "delta.7z"), one, [{"id": py7zr.FILTER_DELTA}, {"id": py7zr.FILTER_LZMA2}])
    make(os.path.join(out, "solid.7z"), [("readme.txt", readme), ("roms/a.bin", rom[:10000]), ("b.bin", rom[10000:])], lz2)
    make(os.path.join(out, "empty.7z"), [], lz2)
    make(os.path.join(out, "bzip2.7z"), one, [{"id": py7zr.FILTER_BZIP2}])
    make(os.path.join(out, "aes.7z"), one, aes, password="fbneo")
    make(os.path.join(out, "aeshdr.7z"), one, aes, password="fbneo", header_encryption=True)
    make(os.path.join(out, "ppmdbig.7z"), one, [{"id": py7zr.FILTER_PPMD, "order": 6, "mem": 30}])
    make(os.path.join(out, "many.7z"), [("%05d.txt" % i, b"") for i in range(70000)] + one, lz2)
    zeros = b"\0" * (513 * 1024 * 1024)
    make(os.path.join(out, "bigblock.7z"), [("rom.bin", rom), ("padding.bin", zeros)], lz2)
    with open(os.path.join(out, "lzma2.7z"), "rb") as f:
        good = f.read()
    with open(os.path.join(out, "truncated.7z"), "wb") as f:
        f.write(good[: len(good) // 2])
    bad = bytearray(good)
    # the packed data starts right after the 32-byte signature header: change a byte in the middle of it
    bad[32 + 40] ^= 0x55
    with open(os.path.join(out, "corrupt.7z"), "wb") as f:
        f.write(bad)

    # the sets
    pac_put = ["--put", "pacman.6e=tests/data/pacman-test.6e", "--put", "82s123.7f=tests/data/pacman-test.7f", "--put",
               "82s126.4a=tests/data/pacman-test.4a", "--put", "pacman.5e=", "--put", "pacman.5f="]
    pon_put = ["--put", "ppokoj1.bin=tests/data/pacman-test.6e", "--put", "82s123.7f=tests/data/pacman-test.7f", "--put",
               "82s126.4a=tests/data/pacman-test.4a", "--put", "ppoko9.bin=", "--put", "ppoko10.bin="]
    make(os.path.join(out, "pacman.7z"), fake_set(headless, "pacman", pac_put)["pacman.zip"], lz2)
    make(os.path.join(out, "ponpoko.7z"), fake_set(headless, "ponpoko", pon_put)["ponpoko.zip"], aes, password="fbneo")
    make(os.path.join(out, "bstarsh.7z"), fake_set(headless, "bstarsh")["bstarsh.zip"], lz2)
    make(os.path.join(out, "puckmanbb.7z"), fake_set(headless, "puckman")["puckman.zip"] + [("padding.bin", zeros)], lz2)
    for n in sorted(os.listdir(out)):
        print("%-14s %7d bytes" % (n, os.path.getsize(os.path.join(out, n))))


if __name__ == "__main__":
    main()
