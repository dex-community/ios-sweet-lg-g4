#!/usr/bin/env python3
"""Turn a framebuffer the shim drew into text.

The shim's console uses the 8x16 font in shim/font.h, scaled 3x, with a
24 px margin and a 24x48 px character cell. This reads such a framebuffer
back into the text it shows, which is how the bring-up logs are read from
a screenshot.

Usage: fb2text.py FILE.ppm|FILE.bin [--bpp 3] [--width 640] [--height 480]
"""
import argparse
import os
import re
import sys

MARGIN = 24
LABEL_COLS = 8


def load_font(path):
    """Parse the font8x16 table out of shim/font.h."""
    src = open(path).read()
    m = re.search(r'font8x16\s*\[[^\]]*\]\s*\[[^\]]*\]\s*=\s*\{(.*?)\n\};', src, re.S)
    if not m:
        sys.exit(f'{path}: font8x16 table not found')
    body = m.group(1)
    rows = re.findall(r'\{\s*((?:0x[0-9a-fA-F]{2}\s*,?\s*){16})\}', body)
    if not rows:
        sys.exit(f'{path}: no glyphs found')
    table = []
    for r in rows:
        vals = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', r)]
        table.append(bytes(vals[:16]))
    return table


def read_ppm(path):
    data = open(path, 'rb').read()
    if data[:2] == b'P6':
        # header: P6 <w> <h> <max>
        fields, i = [], 2
        while len(fields) < 3:
            while i < len(data) and data[i:i + 1].isspace():
                i += 1
            if data[i:i + 1] == b'#':
                while data[i:i + 1] not in (b'\n', b''):
                    i += 1
                continue
            j = i
            while j < len(data) and not data[j:j + 1].isspace():
                j += 1
            fields.append(int(data[i:j]))
            i = j
        i += 1
        w, h = fields[0], fields[1]
        return w, h, data[i:i + w * h * 3]
    # raw RGB
    return None, None, data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--width', type=int, default=640)
    ap.add_argument('--height', type=int, default=480)
    ap.add_argument('--font', default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                   '../shim/font.h'))
    ap.add_argument('--scale', type=int, default=3)
    ap.add_argument('--bpp', type=int, default=3, choices=[3, 4],
                    help='bytes per pixel in the dump (3 = r8g8b8, 4 = xrgb8888)')
    args = ap.parse_args()

    w, h, rgb = read_ppm(args.file)
    if w:
        args.width, args.height = w, h

    # The shim writes 0xAARRGGBB as a little-endian word, so a *raw* 32bpp
    # dump holds B,G,R,A in memory. A PPM is already RGB.
    if args.bpp == 4 and w is None:
        out = bytearray()
        for i in range(0, len(rgb) - 3, 4):
            out += bytes((rgb[i + 2], rgb[i + 1], rgb[i]))
        rgb = bytes(out)
    font = load_font(args.font)
    scale = args.scale
    ch_w, ch_h = 8 * scale, 16 * scale

    # The most common colour is the background.
    from collections import Counter
    counts = Counter(rgb[i:i + 3] for i in range(0, len(rgb) - 2, 3))
    bg = counts.most_common(1)[0][0]
    print(f'# {args.width}x{args.height}, background {bg.hex()}')

    def cell_bitmap(cx, cy):
        """Return the 16x8 bitmap of the character at cell (cx, cy)."""
        bits = bytearray(16)
        for gy in range(16):
            byte = 0
            for gx in range(8):
                x = MARGIN + cx * ch_w + gx * scale
                y = MARGIN + cy * ch_h + gy * scale
                if x >= args.width or y >= args.height:
                    continue
                off = (y * args.width + x) * 3
                if rgb[off:off + 3] != bg:
                    byte |= 0x80 >> gx
            bits[gy] = byte
        return bytes(bits)

    lookup = {}
    for i, glyph in enumerate(font):
        lookup.setdefault(glyph, chr(0x20 + i))

    cols = (args.width - MARGIN) // ch_w
    rows = (args.height - MARGIN) // ch_h
    for cy in range(rows):
        line = []
        for cx in range(cols):
            line.append(lookup.get(cell_bitmap(cx, cy), ' ' if cell_bitmap(cx, cy) != bytes(16) else ''))
        text = ''.join(line).rstrip()
        if text.strip():
            print(f'{cy:3} |{text}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
