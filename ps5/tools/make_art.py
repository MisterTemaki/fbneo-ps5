#!/usr/bin/env python3
# FBNeo PS5: the app's provisional background, drawn from scratch (no logos, no game art): background-source.png
# and pic0.dds / pic1.dds (3840x2160 BC7 DX10 DDS without mipmaps, through bc7enc_rdo when it is given). With
# --icon it also draws a provisional icon (icon-source.png, icon0.png); the app's icon is now the project's own art.
#   make_art.py <out dir> <font.ttf> [bc7enc] [--icon]
# SPDX-License-Identifier: MIT
import math
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

out, font_path = sys.argv[1], sys.argv[2]
args = [a for a in sys.argv[3:] if a != '--icon']
bc7enc = args[0] if args else None
want_icon = '--icon' in sys.argv


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient(w, h, stops):
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        t = y / (h - 1)
        for i in range(len(stops) - 1):
            if stops[i][0] <= t <= stops[i + 1][0]:
                u = (t - stops[i][0]) / max(1e-9, stops[i + 1][0] - stops[i][0])
                c = lerp(stops[i][1], stops[i + 1][1], u)
                break
        for x in range(w):
            px[x, y] = c
    return im


def synth_scene(w, h, horizon=0.58, text=True, scale=1.0):
    """Night sky, a striped sun, a neon grid floor."""
    im = gradient(w, h, [(0.0, (8, 6, 26)), (horizon, (52, 16, 84)), (horizon + 0.001, (18, 6, 34)), (1.0, (6, 2, 14))])
    d = ImageDraw.Draw(im, 'RGBA')
    hy = int(h * horizon)
    # stars
    import random
    rnd = random.Random(1234)
    for _ in range(int(w * h / 9000)):
        x, y = rnd.randrange(w), rnd.randrange(int(hy * 0.9))
        r = rnd.choice([1, 1, 1, 2]) * scale
        a = rnd.randrange(90, 230)
        d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 255, a))
    # the sun: warm bands, cut by horizontal gaps that widen towards the horizon
    sr = int(min(w, h) * 0.24)
    cx, cy = int(w * 0.5), hy - int(sr * 0.35)
    sun = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sun)
    for y in range(cy - sr, hy):
        t = (y - (cy - sr)) / (2 * sr)
        c = lerp((255, 214, 92), (255, 46, 136), min(1, t * 1.3))
        half = math.sqrt(max(0, sr * sr - (y - cy) ** 2))
        sd.line([(cx - half, y), (cx + half, y)], fill=c + (255,))
    k = 0
    y = cy - int(sr * 0.1)
    gap = 2 * scale
    while y < hy:
        sd.rectangle([0, y, w, y + gap], fill=(0, 0, 0, 0))
        y += int(sr * 0.11) - k
        gap += 2 * scale
        k += int(2 * scale)
        if sr * 0.11 - k < 6:
            break
    glow = sun.filter(ImageFilter.GaussianBlur(sr * 0.18))
    im.paste(glow, (0, 0), glow)
    im.paste(sun, (0, 0), sun)
    # the floor grid in perspective
    d = ImageDraw.Draw(im, 'RGBA')
    line = (255, 60, 200, 210)
    lw = max(1, int(3 * scale))
    for i in range(1, 26):
        t = (i / 26) ** 2.2
        y = hy + int((h - hy) * t)
        d.line([(0, y), (w, y)], fill=line, width=lw)
    vx = w / 2
    for i in range(-30, 31):
        x0 = vx + i * w * 0.06
        x1 = vx + i * w * 0.6
        d.line([(x0, hy), (x1, h)], fill=line, width=lw)
    d.line([(0, hy), (w, hy)], fill=(255, 140, 230, 255), width=lw * 2)
    return im


def scanlines(im, step, alpha):
    d = ImageDraw.Draw(im, 'RGBA')
    for y in range(0, im.height, step):
        d.line([(0, y), (im.width, y)], fill=(0, 0, 0, alpha))
    return im


def text_outline(d, xy, s, font, fill, outline, width):
    x, y = xy
    for dx in range(-width, width + 1):
        for dy in range(-width, width + 1):
            if dx * dx + dy * dy <= width * width:
                d.text((x + dx, y + dy), s, font=font, fill=outline)
    d.text((x, y), s, font=font, fill=fill)


def arcade_stick(d, cx, cy, s):
    """A joystick and three buttons, seen from the front: the control panel of an arcade cabinet."""
    # panel
    d.rounded_rectangle([cx - 2.1 * s, cy + 0.55 * s, cx + 2.1 * s, cy + 1.35 * s], radius=0.18 * s,
                        fill=(30, 22, 60, 255), outline=(120, 220, 255, 255), width=max(2, int(0.05 * s)))
    # stick: shaft and ball
    d.rectangle([cx - 1.25 * s - 0.07 * s, cy - 0.15 * s, cx - 1.25 * s + 0.07 * s, cy + 0.7 * s], fill=(210, 210, 225, 255))
    d.ellipse([cx - 1.25 * s - 0.42 * s, cy + 0.6 * s, cx - 1.25 * s + 0.42 * s, cy + 0.82 * s], fill=(15, 10, 30, 255))
    d.ellipse([cx - 1.25 * s - 0.36 * s, cy - 0.62 * s, cx - 1.25 * s + 0.36 * s, cy + 0.1 * s], fill=(255, 52, 96, 255))
    d.ellipse([cx - 1.25 * s - 0.2 * s, cy - 0.5 * s, cx - 1.25 * s - 0.02 * s, cy - 0.32 * s], fill=(255, 170, 190, 255))
    # buttons
    for i, col in enumerate([(255, 214, 92), (80, 230, 255), (120, 255, 150)]):
        bx = cx - 0.15 * s + i * 0.75 * s
        by = cy + 0.62 * s
        d.ellipse([bx - 0.3 * s, by - 0.12 * s, bx + 0.3 * s, by + 0.2 * s], fill=(10, 8, 20, 255))
        d.ellipse([bx - 0.3 * s, by - 0.26 * s, bx + 0.3 * s, by + 0.06 * s], fill=col + (255,))


def icon(size):
    im = synth_scene(size, size, horizon=0.62, scale=size / 512)
    d = ImageDraw.Draw(im, 'RGBA')
    s = size * 0.17
    arcade_stick(d, size * 0.5, size * 0.47, s)
    f = ImageFont.truetype(font_path, int(size * 0.2))
    t = 'FBNeo'
    tw = d.textlength(t, font=f)
    text_outline(d, ((size - tw) / 2, size * 0.04), t, f, (255, 255, 255, 255), (60, 10, 90, 255), max(2, size // 90))
    f2 = ImageFont.truetype(font_path, int(size * 0.11))
    t2 = 'PS5'
    tw2 = d.textlength(t2, font=f2)
    text_outline(d, ((size - tw2) / 2, size * 0.79), t2, f2, (120, 230, 255, 255), (20, 6, 40, 255), max(2, size // 120))
    return scanlines(im, max(2, size // 170), 40)


def background(w, h):
    im = synth_scene(w, h, horizon=0.6, scale=w / 1920)
    d = ImageDraw.Draw(im, 'RGBA')
    arcade_stick(d, w * 0.5, h * 0.74, h * 0.09)
    return scanlines(im, max(2, h // 540), 34)


os.makedirs(out, exist_ok=True)
if want_icon:
    ic = icon(1254)
    ic.save(os.path.join(out, 'icon-source.png'))
    ic.resize((512, 512), Image.LANCZOS).save(os.path.join(out, 'icon0.png'))
bg = background(3840, 2160)
bg.resize((1672, 941), Image.LANCZOS).save(os.path.join(out, 'background-source.png'))
if bc7enc:
    tmp = tempfile.mkdtemp()
    src = os.path.join(tmp, 'bg.png')
    bg.save(src)
    subprocess.run([bc7enc, '-q', '-g', src, os.path.join(tmp, 'bg.dds')], check=True, cwd=tmp)
    # the header the console's home screen takes (as the boilerplate's prepare-assets.sh writes it): mip count 1
    # (with its flag), depth 1, DX10 alpha mode "straight"
    import struct
    dds = bytearray(open(os.path.join(tmp, 'bg.dds'), 'rb').read())
    assert dds[:4] == b'DDS ' and dds[84:88] == b'DX10'
    flags = struct.unpack_from('<I', dds, 8)[0] | 0x20000
    struct.pack_into('<I', dds, 8, flags)
    struct.pack_into('<I', dds, 24, 1)
    struct.pack_into('<I', dds, 28, 1)
    struct.pack_into('<I', dds, 144, 1)
    for name in ('pic0.dds', 'pic1.dds'):
        open(os.path.join(out, name), 'wb').write(dds)
    shutil.rmtree(tmp)
print('art written to', out)
