#!/usr/bin/env python3
# FBNeo PS5 2.2: synthetic hard disk images for the Killer Instinct tests (no real CHD is ever used or downloaded).
#
#   make_chd.py <out> [--cyls N] [--heads N] [--secs N] [--seed S] [--zlib] [--zero-sha1] [--raw]
#
# A MAME CHD v5 hard disk (GDDD metadata "CYLS:..,HEADS:..,SECS:..,BPS:512", 4 KiB hunks of 8 sectors): uncompressed
# (its map: one 32-bit hunk number per hunk), or with --zlib each hunk deflated (raw deflate, as MAME's "zlib" codec)
# behind a compressed map (Huffman-coded types, lengths, CRC-16s: libchdr's decompress_v5_map read backwards).
# --zero-sha1: the header's SHA-1s left at zero (as a v1/v2 CHD has none); otherwise rawsha1 = SHA-1 of the data and
# sha1 = MAME's combined SHA-1 (rawsha1 + the metadata's tag and SHA-1). --raw: the bare sectors (a .img) instead.
# Sector n holds n (4 bytes, little-endian), then bytes made from n and the seed: every read can be checked.
import hashlib
import struct
import sys
import zlib


def sector(n, seed):
    head = struct.pack('<I', n)
    body = bytes(((n * 7 + i * 13 + seed * 31) & 0xff) for i in range(508))
    return head + body


def crc16(data):
    crc = 0xffff
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, value, bits):
        for i in range(bits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc = self.n = 0

    def done(self):
        if self.n:
            self.out.append(self.acc << (8 - self.n))
            self.acc = self.n = 0
        return bytes(self.out)


def main():
    args = sys.argv[1:]
    out = args[0]

    def opt(name, default):
        return int(args[args.index(name) + 1]) if name in args else default

    cyls, heads, secs, seed = opt('--cyls', 8), opt('--heads', 13), opt('--secs', 47), opt('--seed', 1)
    nsec = cyls * heads * secs
    data = b''.join(sector(n, seed) for n in range(nsec))
    if '--raw' in args:
        open(out, 'wb').write(data)
        return
    hunkbytes, unitbytes = 4096, 512
    hunks = (len(data) + hunkbytes - 1) // hunkbytes
    data_padded = data + b'\0' * (hunks * hunkbytes - len(data))
    meta = ('CYLS:%d,HEADS:%d,SECS:%d,BPS:512' % (cyls, heads, secs)).encode() + b'\0'
    use_zlib = '--zlib' in args

    header_len = 124
    mapoffset = header_len
    if use_zlib:
        comp = []
        for h in range(hunks):
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            raw = data_padded[h * hunkbytes:(h + 1) * hunkbytes]
            comp.append((c.compress(raw) + c.flush(), crc16(raw)))
        lengthbits = max(len(c) for c, _ in comp).bit_length()
        bits = Bits()
        for _ in range(16):  # the Huffman tree: 16 codes of 4 bits (canonical: code = symbol)
            bits.put(4, 4)
        for _ in range(hunks):
            bits.put(0, 4)  # COMPRESSION_TYPE_0: compressors[0], zlib
        for c, crc in comp:
            bits.put(len(c), lengthbits)
            bits.put(crc, 16)
        mapbits = bits.done()
        maplen = 16 + len(mapbits)
    else:
        maplen = 4 * hunks
    metaoffset = mapoffset + maplen
    meta_entry = struct.pack('>I', 0x47444444) + struct.pack('>I', (0x01 << 24) | len(meta)) + struct.pack('>Q', 0) + meta
    end = metaoffset + len(meta_entry)
    first = (end + hunkbytes - 1) // hunkbytes * hunkbytes  # data aligned (an uncompressed map counts in hunks)
    if use_zlib:
        body = b''.join(c for c, _ in comp)
        rawmap = bytearray()
        off = first
        for c, crc in comp:
            rawmap += bytes([0]) + struct.pack('>I', len(c))[1:] + struct.pack('>Q', off)[2:] + struct.pack('>H', crc)
            off += len(c)
        maphead = struct.pack('>I', len(mapbits)) + struct.pack('>Q', first)[2:] + struct.pack('>H', crc16(bytes(rawmap)))
        maphead += bytes([lengthbits, 0, 0, 0])
        mapdata = maphead + mapbits
        compressors = [0x7a6c6962, 0, 0, 0]
    else:
        body = data_padded
        mapdata = b''.join(struct.pack('>I', first // hunkbytes + h) for h in range(hunks))
        compressors = [0, 0, 0, 0]
    if '--zero-sha1' in args:
        rawsha1 = sha1 = b'\0' * 20
    else:
        rawsha1 = hashlib.sha1(data).digest()
        sha1 = hashlib.sha1(rawsha1 + struct.pack('>I', 0x47444444) + hashlib.sha1(meta).digest()).digest()
    header = b'MComprHD' + struct.pack('>II', header_len, 5) + struct.pack('>IIII', *compressors)
    header += struct.pack('>QQQ', len(data), mapoffset, metaoffset) + struct.pack('>II', hunkbytes, unitbytes)
    header += rawsha1 + sha1 + b'\0' * 20
    assert len(header) == header_len
    blob = header + mapdata + meta_entry
    blob += b'\0' * (first - len(blob)) + body
    open(out, 'wb').write(blob)
    print('%s: CHD v5, %s, %d sectors, sha1 %s' % (out, 'zlib' if use_zlib else 'uncompressed', nsec, sha1.hex()))


main()
