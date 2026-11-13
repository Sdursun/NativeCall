#!/usr/bin/env python3
"""Draws the NCStatus icon and writes it as an Amiga .info file with a GlowIcons image
(OS 3.5+ 'FORM ICON': 46x46, 32 colours, a second image with the glow for the selected state)
and a 4 colour planar image for older systems.

    python3 mkicon.py NCStatus.info [preview.png]

The picture: a chip with pins, and on it four bars - the load of the four ARM cores."""
import struct
import sys
import zlib

W = H = 46

PALETTE = [
    (0, 0, 0),          # 0 transparent
    (16, 18, 24),       # 1 outline
    (44, 50, 62),       # 2 chip body, dark
    (60, 68, 84),       # 3
    (78, 88, 108),      # 4
    (98, 110, 132),     # 5 chip body, light
    (128, 142, 166),    # 6 chip edge highlight
    (10, 14, 20),       # 7 display background
    (24, 32, 44),       # 8 display grid
    (94, 98, 104),      # 9 pin, dark
    (150, 155, 162),    # 10 pin
    (206, 210, 216),    # 11 pin, light
    (250, 250, 250),    # 12 white
    (18, 110, 44),      # 13 green, dark
    (40, 176, 70),      # 14 green
    (120, 230, 130),    # 15 green, light
    (150, 140, 10),     # 16 yellow, dark
    (232, 208, 30),     # 17 yellow
    (255, 240, 130),    # 18 yellow, light
    (170, 84, 8),       # 19 orange, dark
    (242, 140, 24),     # 20 orange
    (255, 196, 110),    # 21 orange, light
    (140, 20, 20),      # 22 red, dark
    (226, 48, 44),      # 23 red
    (255, 140, 130),    # 24 red, light
    (255, 250, 200),    # 25 glow, inner
    (255, 228, 96),     # 26 glow
    (255, 190, 40),     # 27 glow
    (236, 140, 20),     # 28 glow, outer
    (70, 150, 230),     # 29 blue dot (pin 1 mark)
    (160, 210, 255),    # 30 blue, light
    (34, 40, 50),       # 31 chip shadow
]
assert len(PALETTE) == 32


def draw():
    p = [[0] * W for _ in range(H)]

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            p[y][x] = c

    def rect(x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                put(x, y, c)

    # pins: seven on each side
    for i in range(7):
        a = 12 + i * 3
        for (x0, y0, x1, y1) in ((a, 4, a + 1, 9), (a, 36, a + 1, 41), (4, a, 9, a + 1), (36, a, 41, a + 1)):
            rect(x0, y0, x1, y1, 10)
        # light and shade on each pin
        rect(a, 4, a, 8, 11)
        rect(a, 37, a, 41, 11)
        rect(a + 1, 41, a + 1, 41, 9)
        rect(4, a, 8, a, 11)
        rect(37, a, 41, a, 11)
        rect(41, a + 1, 41, a + 1, 9)
        put(a + 1, 4, 9)
        put(4, a + 1, 9)

    # body: rounded square with a diagonal shade
    x0, y0, x1, y1 = 9, 9, 36, 36
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            corner = (x in (x0, x1) and y in (y0, y1))
            if corner:
                continue
            d = (x - x0) + (y - y0)
            c = 5 if d < 14 else 4 if d < 26 else 3 if d < 40 else 2
            put(x, y, c)
    for i in range(x0 + 1, x1):
        put(i, y0, 6)
        put(x0, i, 6)
        put(i, y1, 31)
        put(x1, i, 31)
    # outline
    for i in range(x0 + 1, x1):
        for (x, y) in ((i, y0 - 1), (i, y1 + 1), (x0 - 1, i), (x1 + 1, i)):
            if p[y][x] == 0:
                put(x, y, 1)

    # pin 1 mark
    put(12, 12, 29)
    put(13, 12, 30)
    put(12, 13, 29)
    put(13, 13, 29)

    # display with the four bars
    dx0, dy0, dx1, dy1 = 14, 16, 31, 33
    rect(dx0, dy0, dx1, dy1, 7)
    for y in range(dy0 + 2, dy1, 4):
        for x in range(dx0 + 1, dx1):
            put(x, y, 8)
    for i in range(dx0, dx1 + 1):
        put(i, dy0 - 1, 1)
        put(i, dy1 + 1, 6)
    for i in range(dy0, dy1 + 1):
        put(dx0 - 1, i, 1)
        put(dx1 + 1, i, 6)
    bars = ((6, (13, 14, 15)), (10, (16, 17, 18)), (13, (19, 20, 21)), (16, (22, 23, 24)))
    for n, (height, (dark, mid, light)) in enumerate(bars):
        bx = dx0 + 1 + n * 4
        top = dy1 - height + 1
        rect(bx, top, bx + 2, dy1, mid)
        rect(bx, top, bx, dy1, light)
        rect(bx + 2, top, bx + 2, dy1, dark)
        put(bx + 1, top, light)
    return p


def glow(p):
    """The selected image: three rings of light around everything opaque."""
    q = [row[:] for row in p]
    dist = [[0 if p[y][x] else 99 for x in range(W)] for y in range(H)]
    for step in range(1, 4):
        for y in range(H):
            for x in range(W):
                if dist[y][x] == 99:
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            yy, xx = y + dy, x + dx
                            if 0 <= yy < H and 0 <= xx < W and dist[yy][xx] == step - 1:
                                dist[y][x] = step + 100      # set in this pass
        for y in range(H):
            for x in range(W):
                if dist[y][x] == step + 100:
                    dist[y][x] = step
    for y in range(H):
        for x in range(W):
            if 1 <= dist[y][x] <= 3:
                q[y][x] = (25, 27, 28)[dist[y][x] - 1]
    # the dark outline would look like a hole in the light
    for y in range(H):
        for x in range(W):
            if q[y][x] == 1 and not (15 <= y <= 34 and 13 <= x <= 32):
                q[y][x] = 26
    return q


def planar(p):
    """Two bitplanes for systems without the new icon format: 0 background, 1 black, 2 white, 3 grey/blue."""
    def col(c):
        if c == 0:
            return 0
        r, g, b = PALETTE[c]
        lum = (r * 30 + g * 59 + b * 11) // 100
        return 1 if lum < 70 else 2 if lum > 170 else 3
    row_bytes = ((W + 15) // 16) * 2
    out = bytearray()
    for plane in range(2):
        for y in range(H):
            row = bytearray(row_bytes)
            for x in range(W):
                if (col(p[y][x]) >> plane) & 1:
                    row[x // 8] |= 0x80 >> (x % 8)
            out += row
    return bytes(out)


def image_struct(p):
    return struct.pack('>hhhhhIBBI', 0, 0, W, H, 2, 1, 3, 0, 0) + planar(p)


def chunk(name, data):
    return name + struct.pack('>I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def imag(p):
    pixels = bytes(c for row in p for c in row)
    pal = bytes(v for rgb in PALETTE for v in rgb)
    # transparent colour 0, 32 colours, flags: has transparent colour + has palette,
    # image and palette not compressed, depth 5
    head = struct.pack('>BBBBBBHH', 0, len(PALETTE) - 1, 3, 0, 0, 5, len(pixels) - 1, len(pal) - 1)
    return chunk(b'IMAG', head + pixels + pal)


def info(normal, selected):
    gadget = struct.pack('>IhhhhHHHIIIiIHI', 0, 0, 0, W, H, 6, 3, 1, 1, 1, 0, 0, 0, 0, 1)
    # WBTOOL, no default tool, no tool types, no position, stack 64 KB
    disk_object = struct.pack('>HH', 0xE310, 1) + gadget + \
        struct.pack('>BBIIiiIIi', 3, 0, 0, 0, -0x80000000, -0x80000000, 0, 0, 65536)
    assert len(disk_object) == 78
    face = chunk(b'FACE', struct.pack('>BBBBH', W - 1, H - 1, 1, 0x11, len(PALETTE) * 3 - 1))
    body = b'ICON' + face + imag(normal) + imag(selected)
    form = b'FORM' + struct.pack('>I', len(body)) + body
    return disk_object + image_struct(normal) + image_struct(selected) + form


def png(images, scale, path):
    """A preview: the images side by side on grey and on white, enlarged."""
    backgrounds = ((170, 170, 170), (255, 255, 255))
    width = len(images) * len(backgrounds) * W * scale
    rows = []
    for y in range(H):
        row = bytearray()
        for bg in backgrounds:
            for p in images:
                for x in range(W):
                    rgb = PALETTE[p[y][x]] if p[y][x] else bg
                    row += bytes(rgb) * scale
        rows += [bytes(row)] * scale
    raw = b''.join(b'\0' + r for r in rows)

    def ch(name, data):
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', width, H * scale, 8, 2, 0, 0, 0))
                + ch(b'IDAT', zlib.compress(raw, 9)) + ch(b'IEND', b''))


normal = draw()
selected = glow(normal)
with open(sys.argv[1], 'wb') as f:
    f.write(info(normal, selected))
if len(sys.argv) > 2:
    png((normal, selected), 6, sys.argv[2])
