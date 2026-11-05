#!/usr/bin/env python3
# FBNeo PS5 2.3 tests: two tiny programs for Daytona USA's stand-in sets (no real ROM: make_fake_set.py --put puts
# them in place of the game's), so that a test can follow the controls from the pad to the picture:
#
#   - the Model 1 I/O board's Z80 (epr-14869c.25): over and over, reads the MSM6253's channels 0 (steering) and 1
#     (accelerator) bit by bit and the 315-5338A's port 1 (IN0: coin, start...), and writes them into the dual port
#     RAM at 0x30, 0x32 and 0x31 (through the 315-5338A's serial registers, as the real program does);
#   - the main board's i960 (epr-16722a.12 / epr-16723a.13, the low and high 16-bit halves of each word): fills the
#     colour tables (colorxlat: component c -> c * 8), then over and over makes palette entry 0 (the screen's
#     background) from those three bytes: red = steering >> 3, green = IN0 & 31 (coin 1, coin 2, test, service,
#     start: active low), blue = accelerator >> 3.
#
#   make_model2_standin.py <out dir>   -> i960-lo.bin, i960-hi.bin, io-z80.bin
#
# SPDX-License-Identifier: MIT
import os
import struct
import sys


# ---- i960 (the same encodings as tests/test_cpus.cpp's, which run on MAME's core too) ----------------------------
def g(n):
    return 16 + n


def reg(major, minor, dst, s1, s2, lit1=False, lit2=False):
    return (major << 24) | ((dst & 31) << 19) | ((s2 & 31) << 14) | (0x1000 if lit2 else 0) | (0x800 if lit1 else 0) | \
        (minor << 7) | (s1 & 31)


def cobr(op, s1, s2, disp, lit1=False):
    return (op << 24) | ((s1 & 31) << 19) | ((s2 & 31) << 14) | (0x2000 if lit1 else 0) | (disp & 0x1ffc)


def mema(op, r, abase, offset):
    return (op << 24) | ((r & 31) << 19) | ((abase & 31) << 14) | 0x2000 | (offset & 0xfff)


class I960:
    def __init__(self, base):
        self.base = base
        self.w = []

    def here(self):
        return self.base + 4 * len(self.w)

    def emit(self, x):
        self.w.append(x & 0xffffffff)

    def lda(self, r, v):  # MEMB mode 0xc: the constant in the next word
        self.emit((0x8c << 24) | ((r & 31) << 19) | 0x1000 | (0xc << 10))
        self.emit(v)

    def addo(self, dst, s1, s2, lit1=False):
        self.emit(reg(0x59, 0x0, dst, s1, s2, lit1))

    def shro(self, dst, n, s2):
        self.emit(reg(0x59, 0x8, dst, n, s2, True))

    def shlo(self, dst, n, s2):
        self.emit(reg(0x59, 0xc, dst, n, s2, True))

    def andl(self, dst, n, s2):  # dst = n & s2 (n a literal 0..31)
        self.emit(reg(0x58, 0x1, dst, n, s2, True))

    def orr(self, dst, s1, s2):
        self.emit(reg(0x58, 0x7, dst, s1, s2))

    def stos(self, r, abase, off=0):
        self.emit(mema(0x8a, r, abase, off))

    def ldob(self, r, abase, off=0):
        self.emit(mema(0x80, r, abase, off))

    def cmpobne(self, s1, s2, target):  # branch when s1 != s2
        self.emit(cobr(0x35, s1, s2, target - self.here()))

    def b(self, target):
        self.emit((0x08 << 24) | ((target - self.here()) & 0xffffff))


def i960_program():
    rom = bytearray(0x40000)
    # the boot record: SAT, PRCB, (checksum words), the first instruction; the PRCB's word 6: the first frame
    struct.pack_into('<4I', rom, 0, 0x40, 0x80, 0, 0x200)
    struct.pack_into('<I', rom, 0x80 + 24, 0x00500100)
    a = I960(0x200)
    # colorxlat: red at 0x40 + c * 0x100, green 0x2040 + ..., blue 0x4040 + ... (16-bit entries): c * 8
    a.lda(g(0), 0x01810080)
    a.lda(g(3), 0x01814080)
    a.lda(g(4), 0x01818080)
    a.lda(g(5), 0x200)
    a.lda(g(6), 32)
    a.lda(g(1), 0)
    a.lda(g(2), 0)
    top = a.here()
    a.stos(g(2), g(0))
    a.stos(g(2), g(3))
    a.stos(g(2), g(4))
    a.addo(g(0), g(5), g(0))
    a.addo(g(3), g(5), g(3))
    a.addo(g(4), g(5), g(4))
    a.addo(g(2), 8, g(2), lit1=True)
    a.addo(g(1), 1, g(1), lit1=True)
    a.cmpobne(g(6), g(1), top)
    # then: the dual port RAM's bytes 0x30 (steering), 0x31 (IN0), 0x32 (accelerator) -> palette entry 0. Byte k of
    # the dual port RAM is at 0x01c00000 + (k >> 1) * 4 + (k & 1) * 2 (16 bits of each word, MAME's umask 0x00ff00ff)
    a.lda(g(7), 0x01c00060)
    a.lda(g(12), 0x01800000)
    loop = a.here()
    a.ldob(g(8), g(7), 0)
    a.ldob(g(9), g(7), 2)
    a.ldob(g(10), g(7), 4)
    a.shro(g(8), 3, g(8))
    a.andl(g(9), 31, g(9))
    a.shlo(g(9), 5, g(9))
    a.shro(g(10), 3, g(10))
    a.shlo(g(10), 10, g(10))
    a.orr(g(11), g(8), g(9))
    a.orr(g(11), g(11), g(10))
    a.stos(g(11), g(12))
    a.b(loop)
    for i, w in enumerate(a.w):
        struct.pack_into('<I', rom, 0x200 + 4 * i, w)
    lo = bytearray()
    hi = bytearray()
    for i in range(0, len(rom), 4):
        lo += rom[i:i + 2]
        hi += rom[i + 2:i + 4]
    return bytes(lo[:0x20000]), bytes(hi[:0x20000])


# ---- Z80 -----------------------------------------------------------------------------------------------------------
class Z80:
    def __init__(self):
        self.b = bytearray()
        self.labels = {}
        self.fix = []

    def db(self, *xs):
        self.b += bytes(xs)

    def label(self, name):
        self.labels[name] = len(self.b)

    def addr(self, op, name):  # op nn (call / jp) to a label
        self.db(op, 0, 0)
        self.fix.append((len(self.b) - 2, name))

    def done(self):
        for at, name in self.fix:
            v = self.labels[name]
            self.b[at] = v & 0xff
            self.b[at + 1] = v >> 8
        return bytes(self.b)


def z80_program():
    z = Z80()
    z.db(0x31, 0x00, 0x60)           # ld sp,6000h
    z.db(0x3e, 0x06)                 # ld a,06h: ports 1 and 2 are inputs
    z.db(0x32, 0x08, 0x80)           # ld (8008h),a
    z.label('loop')
    z.db(0x1e, 0x00)                 # ld e,0 (steering)
    z.db(0x21, 0x30, 0x00)           # ld hl,0030h
    z.addr(0xcd, 'adc')              # call adc
    z.db(0x1e, 0x01)                 # ld e,1 (accelerator)
    z.db(0x21, 0x32, 0x00)           # ld hl,0032h
    z.addr(0xcd, 'adc')
    z.db(0x3a, 0x01, 0x80)           # ld a,(8001h): IN0
    z.db(0x57)                       # ld d,a
    z.db(0x21, 0x31, 0x00)           # ld hl,0031h
    z.addr(0xcd, 'write')
    z.addr(0xc3, 'loop')             # jp loop
    # adc: channel E -> D, then written at HL
    z.label('adc')
    z.db(0x06, 0xc0)                 # ld b,0c0h
    z.db(0x4b)                       # ld c,e
    z.db(0x02)                       # ld (bc),a: the conversion of channel E (into the shift register)
    z.db(0x16, 0x00)                 # ld d,0
    for _ in range(8):
        z.db(0x0a)                   # ld a,(bc): the next bit (top first)
        z.db(0xe6, 0x01)             # and 1
        z.db(0xcb, 0x22)             # sla d
        z.db(0xb2)                   # or d
        z.db(0x57)                   # ld d,a
    # write: D at dual port RAM address HL (315-5338A: serial output, then commands 0 / 1 / 7)
    z.label('write')
    z.db(0x7d)                       # ld a,l
    z.db(0x32, 0x0a, 0x80)           # ld (800ah),a
    z.db(0x3e, 0x00)                 # ld a,0: the address' low byte
    z.db(0x32, 0x09, 0x80)           # ld (8009h),a
    z.db(0x7c)                       # ld a,h
    z.db(0x32, 0x0a, 0x80)
    z.db(0x3e, 0x01)                 # ld a,1: its high byte
    z.db(0x32, 0x09, 0x80)
    z.db(0x7a)                       # ld a,d
    z.db(0x32, 0x0a, 0x80)
    z.db(0x3e, 0x07)                 # ld a,7: write
    z.db(0x32, 0x09, 0x80)
    z.db(0xc9)                       # ret
    rom = bytearray(0x10000)
    p = z.done()
    rom[:len(p)] = p
    return bytes(rom)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    lo, hi = i960_program()
    open(os.path.join(out, 'i960-lo.bin'), 'wb').write(lo)
    open(os.path.join(out, 'i960-hi.bin'), 'wb').write(hi)
    open(os.path.join(out, 'io-z80.bin'), 'wb').write(z80_program())


if __name__ == '__main__':
    main()
