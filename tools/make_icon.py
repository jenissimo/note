#!/usr/bin/env python3
"""Draw note's application icon and write assets/note.ico.

The icon is generated rather than committed as an opaque blob so it can be
adjusted by editing numbers here.  Nothing outside the standard library is
used: an .ico is a small header plus either a bottom-up BGRA bitmap or, from
Vista onwards, a PNG, and zlib is all a PNG needs.

The drawing is a page with a coloured band across its top and three lines of
text under it.  A light page keeps the silhouette legible against both a dark
taskbar and a light Explorer window, which a dark icon would not; the band
carries the colour so the shape still reads at 16 pixels, where anything finer
turns to mush.

On macOS the same drawing is wanted in a different shape: the system draws
application icons as a rounded square sitting in a margin, so the page grows
into that square rather than keeping its portrait proportions.  It is the same
icon -- light page, coloured band, three lines -- cut to the shape the Dock
expects, and it leaves through .icns instead of .ico.

Usage:  python tools/make_icon.py [assets/note.ico]
        python tools/make_icon.py --icns [assets/note.icns]     (macOS only)
        python tools/make_icon.py --iconset DIR
"""

import os
import struct
import subprocess
import sys
import zlib

# Colours, straight from the editor's own light palette.
PAGE   = (250, 250, 249)
BORDER = (172, 169, 163)
ACCENT = (74, 144, 217)
LINE   = (126, 132, 141)

# Supersampling factor; coverage is counted, not filtered.  Small icons want
# all of it, because that is where a rounded corner lives or dies, but 8x8
# samples of a 1024 canvas is 67 million of them and pure Python takes minutes
# over that.  Sampling to a fixed resolution instead keeps every size honest
# and every size quick: the big ones have pixels to spare for their own edges.
SS_MAX = 8
SS_GRID = 2048


def ss_for(size):
    return max(2, min(SS_MAX, -(-SS_GRID // size)))


# Geometry in fractions of the canvas.  Windows draws the page edge to edge;
# macOS wants it as the rounded square of its own icon grid, which is 824 of
# 1024 across with a corner radius of 185 -- hence the 0.098/0.902/0.181.
# Drawn any larger it sits taller than its neighbours in the Dock.
SHAPE = {
    "win": (0.13, 0.07, 0.87, 0.93, 0.11),
    "mac": (0.0977, 0.0977, 0.9023, 0.9023, 0.181),
}
BAND_H   = 0.22   # of the page height
LINE_PAD = 0.14   # inset from the page edge, of the page width

ICONSET = [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2),
           (256, 1), (256, 2), (512, 1), (512, 2)]


def rounded_inside(px, py, x0, y0, x1, y1, r):
    """Is the point inside a rounded rectangle?"""
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    hx, hy = (x1 - x0) / 2.0, (y1 - y0) / 2.0
    dx = abs(px - cx) - (hx - r)
    dy = abs(py - cy) - (hy - r)
    if dx <= 0 and dy <= 0:
        return True
    dx = max(dx, 0.0)
    dy = max(dy, 0.0)
    return (dx * dx + dy * dy) <= r * r


def over(dst, src, a):
    """Source-over compositing of one straight-alpha colour."""
    r, g, b, da = dst
    sr, sg, sb = src
    na = a + da * (1 - a)
    if na <= 0:
        return (0, 0, 0, 0.0)
    nr = (sr * a + r * da * (1 - a)) / na
    ng = (sg * a + g * da * (1 - a)) / na
    nb = (sb * a + b * da * (1 - a)) / na
    return (nr, ng, nb, na)


def render(size, shape="win"):
    """An RGBA pixel list, row-major from the top."""
    px = [(0, 0, 0, 0.0)] * (size * size)
    PAGE_X0, PAGE_Y0, PAGE_X1, PAGE_Y1, PAGE_R = SHAPE[shape]
    SS = ss_for(size)

    band_y1 = PAGE_Y0 + (PAGE_Y1 - PAGE_Y0) * BAND_H
    pw = PAGE_X1 - PAGE_X0
    lx0 = PAGE_X0 + pw * LINE_PAD
    lx1 = PAGE_X1 - pw * LINE_PAD

    # Three text lines, the last one short the way a paragraph ends.
    line_h = (PAGE_Y1 - PAGE_Y0) * 0.085
    gap = (PAGE_Y1 - band_y1 - line_h * 3) / 4.0
    lines = []
    y = band_y1 + gap
    for i in range(3):
        right = lx1 if i < 2 else lx0 + (lx1 - lx0) * 0.55
        lines.append((lx0, y, right, y + line_h))
        y += line_h + gap

    # A hairline border only helps once there are pixels to spare for it.
    border = 1.0 / size if size >= 32 else 0.0

    for iy in range(size):
        for ix in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    u = (ix + (sx + 0.5) / SS) / size
                    v = (iy + (sy + 0.5) / SS) / size

                    c = (0, 0, 0, 0.0)
                    if rounded_inside(u, v, PAGE_X0, PAGE_Y0,
                                      PAGE_X1, PAGE_Y1, PAGE_R):
                        c = over(c, PAGE, 1.0)

                        if border and not rounded_inside(
                                u, v,
                                PAGE_X0 + border, PAGE_Y0 + border,
                                PAGE_X1 - border, PAGE_Y1 - border,
                                max(PAGE_R - border, 0.0)):
                            c = over(c, BORDER, 1.0)

                        elif v < band_y1:
                            c = over(c, ACCENT, 1.0)

                        else:
                            for (ax0, ay0, ax1, ay1) in lines:
                                if ax0 <= u <= ax1 and ay0 <= v <= ay1:
                                    c = over(c, LINE, 1.0)
                                    break

                    acc[0] += c[0] * c[3]
                    acc[1] += c[1] * c[3]
                    acc[2] += c[2] * c[3]
                    acc[3] += c[3]

            n = float(SS * SS)
            a = acc[3] / n
            if a <= 0:
                px[iy * size + ix] = (0, 0, 0, 0)
            else:
                px[iy * size + ix] = (
                    int(round(acc[0] / acc[3])),
                    int(round(acc[1] / acc[3])),
                    int(round(acc[2] / acc[3])),
                    int(round(a * 255)),
                )
    return px


def as_bmp(px, size):
    """A bottom-up 32-bit DIB with the AND mask an .ico still expects."""
    header = struct.pack("<IiiHHIIiiII",
                         40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    body = bytearray()
    for iy in range(size - 1, -1, -1):
        for ix in range(size):
            r, g, b, a = px[iy * size + ix]
            body += bytes((b, g, r, a))
    # Alpha does the masking; the mask stays zero but must still be there.
    mask_row = ((size + 31) // 32) * 4
    body += bytes(mask_row * size)
    return header + bytes(body)


def as_png(px, size):
    raw = bytearray()
    for iy in range(size):
        raw.append(0)                       # filter: none
        for ix in range(size):
            r, g, b, a = px[iy * size + ix]
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def write_iconset(directory):
    """The ten PNGs macOS names by point size and scale, ready for iconutil."""
    if not os.path.isdir(directory):
        os.makedirs(directory)
    drawn = {}      # 32, 256 and 512 pixels are each asked for twice
    for points, scale in ICONSET:
        size = points * scale
        if size not in drawn:
            drawn[size] = as_png(render(size, "mac"), size)
        name = "icon_%dx%d%s.png" % (points, points,
                                     "@2x" if scale == 2 else "")
        with open(os.path.join(directory, name), "wb") as f:
            f.write(drawn[size])
        print("  %s (%d px)" % (name, size))
    return directory


def make_icns(out):
    """Draw the iconset next to the target and let iconutil fold it up.

    iconutil ships with macOS and is the only supported way to write an .icns;
    there is no point reimplementing its container when every Mac has it.
    """
    stem = out[:-5] if out.endswith(".icns") else out
    directory = stem + ".iconset"
    parent = os.path.dirname(out)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    write_iconset(directory)
    subprocess.check_call(["iconutil", "-c", "icns", "-o", out, directory])
    print("wrote %s: %d images, %d bytes"
          % (out, len(ICONSET), os.path.getsize(out)))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--iconset":
        write_iconset(sys.argv[2] if len(sys.argv) > 2
                      else "build/note.iconset")
        return
    if len(sys.argv) > 1 and sys.argv[1] == "--icns":
        make_icns(sys.argv[2] if len(sys.argv) > 2 else "assets/note.icns")
        return

    out = sys.argv[1] if len(sys.argv) > 1 else "assets/note.ico"

    # Only 16 stays a bitmap, as the one entry nothing anywhere can misread.
    # The rest are PNG, which Windows has understood inside an .ico since
    # Vista: a 32x32 bitmap costs 4 KB and the same image as PNG costs a few
    # hundred bytes, which is a bigger saving than dropping every size above
    # 32 would be -- and dropping those is what actually shows, because the
    # shell upscales the largest entry it has for Explorer's large-icon views
    # and for Alt+Tab on a high-DPI display.
    #
    # 128 is left out: 64 upscales to it acceptably and nothing asks for both.
    #
    # 256 is left out too, and that one is a real trade.  Its PNG is 2.2 KB of
    # already-compressed data -- the single largest entry here, and one UPX
    # cannot squeeze any further -- and carrying it is what put the packed
    # build over 64 KB.  Explorer's extra-large view now upscales the 64, which
    # is softer than a drawn 256 but still reads: the icon is a page with a
    # coloured band, not something with fine detail to lose.
    sizes = [(16, "bmp"),
             (20, "png"), (24, "png"), (32, "png"), (48, "png"),
             (64, "png")]

    images = []
    for size, kind in sizes:
        px = render(size)
        images.append((size, as_bmp(px, size) if kind == "bmp"
                             else as_png(px, size)))

    offset = 6 + 16 * len(images)
    directory = struct.pack("<HHH", 0, 1, len(images))
    for size, data in images:
        directory += struct.pack("<BBBBHHII",
                                 size if size < 256 else 0,
                                 size if size < 256 else 0,
                                 0, 0, 1, 32, len(data), offset)
        offset += len(data)

    with open(out, "wb") as f:
        f.write(directory)
        for _, data in images:
            f.write(data)

    total = offset
    print("wrote %s: %d images, %d bytes" % (out, len(images), total))


if __name__ == "__main__":
    main()
