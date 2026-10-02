#!/usr/bin/env python3
"""
gen_ui_assets.py - generate main/display/ui_assets.h

Why a generator instead of hand-written hex
-------------------------------------------
Icon bitmaps written by hand as hex are impossible to review and easy to get
subtly wrong (a flipped bit is an invisible bug until it is on a panel). Here
the icons are defined as geometric primitives and rasterised by this script,
which also prints an ASCII preview of every glyph - so the result can be
checked by eye before it ever reaches the device.

Font
----
The body font is the classic 5x7 bitmap font that ships with Adafruit GFX
(BSD licensed; the same glyph shapes every character-LCD UI has used for
decades). It is embedded here as a compact table rather than invented, because
a hand-drawn font reads badly at 5px.

Storage format matches the existing font8x16.h / oled_char() convention:
one byte per ROW, most significant bit = leftmost pixel.

Run:  python tools/gen_ui_assets.py --check     (preview only)
      python tools/gen_ui_assets.py             (preview + write header)
"""

import argparse
import os
import sys

# --------------------------------------------------------------------------
# 5x7 font: printable ASCII 0x20..0x7F, 5 columns each, column-major byte per
# column with bit0 = top row (the classic font5x7 layout).
# --------------------------------------------------------------------------
FONT5X7 = {
    ' ': [0x00, 0x00, 0x00, 0x00, 0x00],
    '!': [0x00, 0x00, 0x5F, 0x00, 0x00],
    '"': [0x00, 0x07, 0x00, 0x07, 0x00],
    '#': [0x14, 0x7F, 0x14, 0x7F, 0x14],
    '$': [0x24, 0x2A, 0x7F, 0x2A, 0x12],
    '%': [0x23, 0x13, 0x08, 0x64, 0x62],
    '&': [0x36, 0x49, 0x55, 0x22, 0x50],
    "'": [0x00, 0x05, 0x03, 0x00, 0x00],
    '(': [0x00, 0x1C, 0x22, 0x41, 0x00],
    ')': [0x00, 0x41, 0x22, 0x1C, 0x00],
    '*': [0x14, 0x08, 0x3E, 0x08, 0x14],
    '+': [0x08, 0x08, 0x3E, 0x08, 0x08],
    ',': [0x00, 0x50, 0x30, 0x00, 0x00],
    '-': [0x08, 0x08, 0x08, 0x08, 0x08],
    '.': [0x00, 0x60, 0x60, 0x00, 0x00],
    '/': [0x20, 0x10, 0x08, 0x04, 0x02],
    '0': [0x3E, 0x51, 0x49, 0x45, 0x3E],
    '1': [0x00, 0x42, 0x7F, 0x40, 0x00],
    '2': [0x42, 0x61, 0x51, 0x49, 0x46],
    '3': [0x21, 0x41, 0x45, 0x4B, 0x31],
    '4': [0x18, 0x14, 0x12, 0x7F, 0x10],
    '5': [0x27, 0x45, 0x45, 0x45, 0x39],
    '6': [0x3C, 0x4A, 0x49, 0x49, 0x30],
    '7': [0x01, 0x71, 0x09, 0x05, 0x03],
    '8': [0x36, 0x49, 0x49, 0x49, 0x36],
    '9': [0x06, 0x49, 0x49, 0x29, 0x1E],
    ':': [0x00, 0x36, 0x36, 0x00, 0x00],
    ';': [0x00, 0x56, 0x36, 0x00, 0x00],
    '<': [0x08, 0x14, 0x22, 0x41, 0x00],
    '=': [0x14, 0x14, 0x14, 0x14, 0x14],
    '>': [0x00, 0x41, 0x22, 0x14, 0x08],
    '?': [0x02, 0x01, 0x51, 0x09, 0x06],
    '@': [0x32, 0x49, 0x79, 0x41, 0x3E],
    'A': [0x7E, 0x11, 0x11, 0x11, 0x7E],
    'B': [0x7F, 0x49, 0x49, 0x49, 0x36],
    'C': [0x3E, 0x41, 0x41, 0x41, 0x22],
    'D': [0x7F, 0x41, 0x41, 0x22, 0x1C],
    'E': [0x7F, 0x49, 0x49, 0x49, 0x41],
    'F': [0x7F, 0x09, 0x09, 0x01, 0x01],
    'G': [0x3E, 0x41, 0x41, 0x51, 0x32],
    'H': [0x7F, 0x08, 0x08, 0x08, 0x7F],
    'I': [0x00, 0x41, 0x7F, 0x41, 0x00],
    'J': [0x20, 0x40, 0x41, 0x3F, 0x01],
    'K': [0x7F, 0x08, 0x14, 0x22, 0x41],
    'L': [0x7F, 0x40, 0x40, 0x40, 0x40],
    'M': [0x7F, 0x02, 0x04, 0x02, 0x7F],
    'N': [0x7F, 0x04, 0x08, 0x10, 0x7F],
    'O': [0x3E, 0x41, 0x41, 0x41, 0x3E],
    'P': [0x7F, 0x09, 0x09, 0x09, 0x06],
    'Q': [0x3E, 0x41, 0x51, 0x21, 0x5E],
    'R': [0x7F, 0x09, 0x19, 0x29, 0x46],
    'S': [0x46, 0x49, 0x49, 0x49, 0x31],
    'T': [0x01, 0x01, 0x7F, 0x01, 0x01],
    'U': [0x3F, 0x40, 0x40, 0x40, 0x3F],
    'V': [0x1F, 0x20, 0x40, 0x20, 0x1F],
    'W': [0x7F, 0x20, 0x18, 0x20, 0x7F],
    'X': [0x63, 0x14, 0x08, 0x14, 0x63],
    'Y': [0x03, 0x04, 0x78, 0x04, 0x03],
    'Z': [0x61, 0x51, 0x49, 0x45, 0x43],
    '[': [0x00, 0x7F, 0x41, 0x41, 0x00],
    '\\': [0x02, 0x04, 0x08, 0x10, 0x20],
    ']': [0x00, 0x41, 0x41, 0x7F, 0x00],
    '^': [0x04, 0x02, 0x01, 0x02, 0x04],
    '_': [0x40, 0x40, 0x40, 0x40, 0x40],
    '`': [0x00, 0x01, 0x02, 0x04, 0x00],
    'a': [0x20, 0x54, 0x54, 0x54, 0x78],
    'b': [0x7F, 0x48, 0x44, 0x44, 0x38],
    'c': [0x38, 0x44, 0x44, 0x44, 0x20],
    'd': [0x38, 0x44, 0x44, 0x48, 0x7F],
    'e': [0x38, 0x54, 0x54, 0x54, 0x18],
    'f': [0x08, 0x7E, 0x09, 0x01, 0x02],
    'g': [0x08, 0x14, 0x54, 0x54, 0x3C],
    'h': [0x7F, 0x08, 0x04, 0x04, 0x78],
    'i': [0x00, 0x44, 0x7D, 0x40, 0x00],
    'j': [0x20, 0x40, 0x44, 0x3D, 0x00],
    'k': [0x00, 0x7F, 0x10, 0x28, 0x44],
    'l': [0x00, 0x41, 0x7F, 0x40, 0x00],
    'm': [0x7C, 0x04, 0x18, 0x04, 0x78],
    'n': [0x7C, 0x08, 0x04, 0x04, 0x78],
    'o': [0x38, 0x44, 0x44, 0x44, 0x38],
    'p': [0x7C, 0x14, 0x14, 0x14, 0x08],
    'q': [0x08, 0x14, 0x14, 0x18, 0x7C],
    'r': [0x7C, 0x08, 0x04, 0x04, 0x08],
    's': [0x48, 0x54, 0x54, 0x54, 0x20],
    't': [0x04, 0x3F, 0x44, 0x40, 0x20],
    'u': [0x3C, 0x40, 0x40, 0x20, 0x7C],
    'v': [0x1C, 0x20, 0x40, 0x20, 0x1C],
    'w': [0x3C, 0x40, 0x30, 0x40, 0x3C],
    'x': [0x44, 0x28, 0x10, 0x28, 0x44],
    'y': [0x0C, 0x50, 0x50, 0x50, 0x3C],
    'z': [0x44, 0x64, 0x54, 0x4C, 0x44],
    '{': [0x00, 0x08, 0x36, 0x41, 0x00],
    '|': [0x00, 0x00, 0x7F, 0x00, 0x00],
    '}': [0x00, 0x41, 0x36, 0x08, 0x00],
    '~': [0x08, 0x08, 0x2A, 0x1C, 0x08],
}

FONT_FIRST = 0x20
FONT_LAST = 0x7E
FONT_W = 5
FONT_H = 7


def font_rows(ch):
    """Convert the column-major 5x7 glyph into this project's row-major,
    MSB-first byte format (one byte per row)."""
    cols = FONT5X7[ch]
    rows = []
    for y in range(FONT_H):
        bits = 0
        for x in range(FONT_W):
            if cols[x] & (1 << y):
                bits |= 1 << (7 - x)
        rows.append(bits)
    return rows


# --------------------------------------------------------------------------
# Icons. 16x16, drawn from primitives so they are reviewable and consistent.
# Style rules that make a small icon set look deliberate rather than random:
#   - the same 1px stroke weight everywhere
#   - a common visual bounding box (1..14) so nothing looks off-centre
#   - outer shapes rounded where the subject is round, square where it is not
# --------------------------------------------------------------------------
W = 16


class Canvas:
    def __init__(self, w=W, h=W):
        self.w, self.h = w, h
        self.p = [[0] * w for _ in range(h)]

    def px(self, x, y):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.p[y][x] = 1

    def hline(self, x0, x1, y):
        for x in range(min(x0, x1), max(x0, x1) + 1):
            self.px(x, y)

    def vline(self, x, y0, y1):
        for y in range(min(y0, y1), max(y0, y1) + 1):
            self.px(x, y)

    def line(self, x0, y0, x1, y1):
        dx, dy = abs(x1 - x0), abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.px(x0, y0)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def rect(self, x0, y0, x1, y1):
        self.hline(x0, x1, y0)
        self.hline(x0, x1, y1)
        self.vline(x0, y0, y1)
        self.vline(x1, y0, y1)

    def fill(self, x0, y0, x1, y1):
        for y in range(y0, y1 + 1):
            self.hline(x0, x1, y)

    def circle(self, cx, cy, r):
        x, y, d = r, 0, 1 - r
        while x >= y:
            for sx, sy in ((x, y), (y, x), (-x, y), (-y, x),
                           (-x, -y), (-y, -x), (x, -y), (y, -x)):
                self.px(cx + sx, cy + sy)
            y += 1
            if d < 0:
                d += 2 * y + 1
            else:
                x -= 1
                d += 2 * (y - x) + 1

    def ascii(self):
        return ["".join("#" if c else "." for c in row) for row in self.p]


def icon_serial():
    """Terminal / serial port: a prompt box with a chevron and a cursor line."""
    c = Canvas()
    c.rect(1, 2, 14, 13)
    c.hline(1, 14, 4)
    c.line(4, 7, 6, 9)
    c.line(6, 9, 4, 11)
    c.hline(8, 11, 11)
    return c


def icon_bus():
    """Bus / wiring: two rails with drops, the usual 'bus' drawing."""
    c = Canvas()
    c.hline(1, 14, 3)
    c.hline(1, 14, 12)
    for x in (3, 8, 13):
        c.vline(x, 3, 12)
    for x in (3, 8, 13):
        c.px(x, 5)
        c.px(x, 7)
        c.px(x, 9)
    return c


def icon_wave():
    """PWM: a square wave, which is exactly what the measurement looks like."""
    c = Canvas()
    c.hline(1, 4, 10)
    c.vline(4, 4, 10)
    c.hline(4, 8, 4)
    c.vline(8, 4, 10)
    c.hline(8, 12, 10)
    c.vline(12, 4, 10)
    c.hline(12, 14, 4)
    c.hline(1, 14, 13)
    return c


def icon_chip():
    """Target chip: a body with pins, the standard 'device' glyph."""
    c = Canvas()
    c.rect(4, 4, 11, 11)
    c.fill(6, 6, 9, 9)
    for y in (6, 9):
        c.hline(2, 3, y)
        c.hline(12, 13, y)
    for x in (6, 9):
        c.vline(x, 2, 3)
        c.vline(x, 12, 13)
    return c


def icon_list():
    """Capture log: lines of text with a leading marker."""
    c = Canvas()
    for i, y in enumerate((3, 7, 11)):
        c.hline(1, 2, y)
        c.hline(4, 14 - i * 2, y)
    return c


def icon_probe():
    """Debug probe: a probe tip with a signal arc."""
    c = Canvas()
    c.rect(6, 1, 9, 6)
    c.vline(7, 7, 10)
    c.vline(8, 7, 10)
    c.hline(7, 8, 11)
    c.hline(5, 10, 13)
    c.hline(3, 12, 14)
    c.px(2, 9)
    c.px(13, 9)
    return c


def icon_network():
    """WiFi: the familiar fan of arcs plus a dot.
    Drawn from three pixel-stepped arcs rather than a circle rasteriser, so the
    shape is explicit instead of depending on float rounding."""
    c = Canvas()
    arcs = [
        # (left, right, top) - each arc is a shallow pixel-stepped curve
        (1, 13, 8),
        (3, 11, 10),
        (5, 9, 12),
    ]
    for left, right, top in arcs:
        cx = (left + right) / 2.0
        for x in range(left, right + 1):
            # quadratic bow: highest at the centre, dropping at the ends
            t = (x - cx) / ((right - left) / 2.0)
            y = top + int(round(2 * t * t))
            c.px(x, y)
    c.fill(6, 13, 8, 15)
    return c


def icon_lightning():
    """Power / reboot: a bolt inside nothing, centred on the usual box."""
    c = Canvas()
    c.line(9, 1, 5, 8)
    c.hline(5, 9, 8)
    c.line(7, 8, 11, 8)
    c.line(6, 15, 10, 8)
    c.hline(5, 8, 8)
    return c


def icon_usb():
    """USB: plug body on top, two contact pins pointing down (the usual
    orientation in icon sets - pins down, housing up)."""
    c = Canvas()
    c.rect(4, 1, 11, 9)
    c.fill(6, 3, 9, 5)
    c.vline(6, 10, 14)
    c.vline(9, 10, 14)
    return c


def icon_cpu():
    """AI / compute: a chip with a dot matrix, distinct from icon_chip."""
    c = Canvas()
    c.rect(2, 2, 13, 13)
    c.rect(5, 5, 10, 10)
    for x in range(6, 10, 2):
        for y in range(6, 10, 2):
            c.px(x, y)
    return c


def icon_back():
    """Back: a left arrow, which needs no translation."""
    c = Canvas()
    c.hline(2, 13, 8)
    c.line(2, 8, 6, 4)
    c.line(2, 8, 6, 12)
    return c


def icon_folder():
    """Group / submenu: a folder outline."""
    c = Canvas()
    c.hline(2, 6, 4)
    c.vline(2, 4, 13)
    c.vline(6, 4, 6)
    c.hline(6, 7, 6)
    c.vline(7, 6, 6)
    c.rect(2, 6, 13, 13)
    return c


ICONS = [
    ("ICON_BACK",     icon_back),
    ("ICON_SERIAL",   icon_serial),
    ("ICON_BUS",      icon_bus),
    ("ICON_WAVE",     icon_wave),
    ("ICON_LIST",     icon_list),
    ("ICON_PROBE",    icon_probe),
    ("ICON_CHIP",     icon_chip),
    ("ICON_NETWORK",  icon_network),
    ("ICON_LIGHTNING", icon_lightning),
    ("ICON_USB",      icon_usb),
    ("ICON_CPU",      icon_cpu),
    ("ICON_FOLDER",   icon_folder),
]


def preview(name, canvas):
    print("  %s" % name)
    for row in canvas.ascii():
        print("      " + row)


def emit_icons(fh):
    """16x16 icons are TWO bytes wide, because oled_bitmap() uses
    stride = (w + 7) / 8 = 2 for w=16 and reads src[col >> 3] with bit
    (7 - (col & 7)). Packing them as a single 16-bit value and shifting by
    (7 - x) for x >= 8 is exactly the negative-shift bug this replaced."""
    fh.write("/* 16x16 monochrome icons: 16 rows of 2 bytes, MSB-first within each\n"
             " * byte, matching oled_bitmap()'s stride/bit order for w=16. Generated\n"
             " * by tools/gen_ui_assets.py - edit that, not this. */\n")
    for name, fn in ICONS:
        c = fn()
        fh.write("static const uint8_t %s[32] = {\n" % name)
        for row in c.p:
            hi = lo = 0
            for x in range(W):
                if row[x]:
                    if x < 8:
                        hi |= 1 << (7 - x)          # first byte: cols 0..7
                    else:
                        lo |= 1 << (7 - (x - 8))    # second byte: cols 8..15
            fh.write("    0x%02X, 0x%02X,\n" % (hi, lo))
        fh.write("};\n\n")


def emit_font(fh):
    fh.write("/* 5x7 body font, MSB-first, one byte per row (same convention as\n"
             " * font8x16.h). Glyph shapes are the classic Adafruit GFX 5x7 set\n"
             " * (BSD). Indexed from 0x%02X. */\n" % FONT_FIRST)
    fh.write("#define UI_FONT_W 5\n#define UI_FONT_H 7\n")
    fh.write("#define UI_FONT_FIRST 0x%02X\n#define UI_FONT_LAST 0x%02X\n\n"
             % (FONT_FIRST, FONT_LAST))
    fh.write("static const uint8_t ui_font5x7[%d][%d] = {\n"
             % (FONT_LAST - FONT_FIRST + 1, FONT_H))
    for code in range(FONT_FIRST, FONT_LAST + 1):
        rows = font_rows(chr(code))
        fh.write("    { %s },  /* '%s' */\n"
                 % (", ".join("0x%02X" % r for r in rows),
                    chr(code) if chr(code) not in "\\'" else "\\" + chr(code)))
    fh.write("};\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="preview only, write nothing")
    args = ap.parse_args()

    print("=== icon preview ===")
    for name, fn in ICONS:
        preview(name, fn())

    print()
    print("=== font preview (sample) ===")
    for ch in "Aa5.%-":
        rows = font_rows(ch)
        print("  '%s'" % ch)
        for r in rows:
            print("      " + "".join("#" if r & (1 << (7 - x)) else "." for x in range(5)))

    if args.check:
        return 0

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "main", "display", "ui_assets.h")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("/* GENERATED by tools/gen_ui_assets.py - do not edit by hand.\n"
                 " * Regenerate: python tools/gen_ui_assets.py\n"
                 " */\n#pragma once\n#include <stdint.h>\n\n")
        emit_icons(fh)
        emit_font(fh)
    print("\nwrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
