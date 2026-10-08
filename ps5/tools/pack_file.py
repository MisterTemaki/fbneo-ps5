#!/usr/bin/env python3
# FBNeo PS5: packs a file for the installer (ProsperoInstall.cpp unpacks it): "FBZ1", the size (8 bytes, little
# endian), then the file compressed with zlib. FBNeo's eboot.bin is ~30 MB; packed, the payload stays small.
#   pack_file.py <in> <out>
# SPDX-License-Identifier: MIT
import struct
import sys
import zlib

data = open(sys.argv[1], 'rb').read()
packed = b'FBZ1' + struct.pack('<Q', len(data)) + zlib.compress(data, 9)
open(sys.argv[2], 'wb').write(packed)
print('%s: %d -> %d bytes' % (sys.argv[2], len(data), len(packed)))
