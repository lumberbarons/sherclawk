#!/usr/bin/env python3
"""Convert the reference PNG to native OS 9 icon and main-window resources.

Stdlib-only PNG decoding avoids a runtime imaging dependency. Rez output is
build data, not a checked-in bitmap. Pixel colors use the classic 6x6x6 cube
for Finder icons; window art uses full RGB composited over white or Platinum gray.
"""
import argparse
import struct
import zlib
from pathlib import Path


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not PNG')
    at, packed = 8, bytearray()
    while at < len(data):
        size = struct.unpack_from('>I', data, at)[0]
        kind = data[at + 4:at + 8]
        chunk = data[at + 8:at + 8 + size]
        if kind == b'IHDR':
            w, h, depth, color, comp, filt, interlace = struct.unpack('>IIBBBBB', chunk)
            if (depth, color, comp, filt, interlace) != (8, 6, 0, 0, 0):
                raise ValueError('expected noninterlaced 8-bit RGBA PNG')
        if kind == b'IDAT':
            packed.extend(chunk)
        at += size + 12
    raw = zlib.decompress(packed)
    stride = w * 4
    if len(raw) != h * (stride + 1):
        raise ValueError('invalid image length')
    prev = bytearray(stride)
    pixels = bytearray()
    for y in range(h):
        off = y * (stride + 1)
        mode = raw[off]
        row = bytearray(raw[off + 1:off + stride + 1])
        for x in range(stride):
            left = row[x - 4] if x >= 4 else 0
            up = prev[x]
            corner = prev[x - 4] if x >= 4 else 0
            if mode == 1:
                predictor = left
            elif mode == 2:
                predictor = up
            elif mode == 3:
                predictor = (left + up) // 2
            elif mode == 4:
                p = left + up - corner
                distances = [abs(p - left), abs(p - up), abs(p - corner)]
                predictor = [left, up, corner][distances.index(min(distances))]
            elif mode == 0:
                predictor = 0
            else:
                raise ValueError('invalid filter')
            row[x] = (row[x] + predictor) & 255
        pixels.extend(row)
        prev = row
    return w, h, pixels


def scale(w, h, data, n):
    # A small transparent margin keeps claws/lens clear of the icon boundary.
    occupied = [(i // 4 % w, i // 4 // w) for i in range(0, len(data), 4)
                if data[i + 3] > 128]
    left = min(p[0] for p in occupied)
    top = min(p[1] for p in occupied)
    right = max(p[0] for p in occupied) + 1
    bottom = max(p[1] for p in occupied) + 1
    extent = max(right - left, bottom - top)
    left -= (extent - (right - left)) // 2
    top -= (extent - (bottom - top)) // 2
    result = []
    for y in range(n):
        for x in range(n):
            sx = left + int((x - 1 + .5) * extent / (n - 2))
            sy = top + int((y - 1 + .5) * extent / (n - 2))
            result.append(tuple(data[(sy * w + sx) * 4:(sy * w + sx) * 4 + 4])
                          if 0 < x < n - 1 and 0 < y < n - 1 and 0 <= sx < w and 0 <= sy < h
                          else (0, 0, 0, 0))
    return result


def resource(kind, ident, data):
    rows = '\n'.join('    $"' + data[i:i + 32].hex().upper() + '"'
                     for i in range(0, len(data), 32))
    return f"data '{kind}' ({ident}) {{\n{rows}\n}};\n"


def icon(pixels, n):
    mask = bytearray(n * n // 8)
    mono = bytearray(len(mask))
    indexed = bytearray()
    rgb = bytearray()
    alpha = bytearray()
    for i, (r, g, b, a) in enumerate(pixels):
        if a >= 128:
            mask[i // 8] |= 128 >> (i % 8)
            if (r * 299 + g * 587 + b * 114) < 160000:
                mono[i // 8] |= 128 >> (i % 8)
        # Classic standard 8-bit color table begins with the RGB cube.
        indexed.append((5 - round(r / 51)) * 36 + (5 - round(g / 51)) * 6 + 5 - round(b / 51))
        rgb.extend((0, r, g, b))
        alpha.append(a)
    return bytes(mono + mask), bytes(indexed), bytes(rgb), bytes(alpha)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('png')
    parser.add_argument('output')
    args = parser.parse_args()
    w, h, rgba = read_png(args.png)
    out = '/* Generated from art/sherclawk.png; do not edit. */\n'
    chunks = []
    for n, mono_type, color_type, rgb_type, mask_type in [
        (32, 'ICN#', 'icl8', 'il32', 'l8mk'), (16, 'ics#', 'ics8', 'is32', 's8mk')]:
        mono, color, rgb, mask = icon(scale(w, h, rgba, n), n)
        for kind, data in [(mono_type, mono), (color_type, color), (rgb_type, rgb), (mask_type, mask)]:
            out += resource(kind, 128, data)
            out += resource(kind, -16455, data)
            chunks.append(kind.encode() + struct.pack('>I', len(data) + 8) + data)
    family = b''.join(chunks)
    out += resource('icns', 128, b'icns' + struct.pack('>I', len(family) + 8) + family)
    out += resource('icns', -16455, b'icns' + struct.pack('>I', len(family) + 8) + family)
    art = struct.pack('>HH', 156, 156)
    for r, g, b, a in scale(w, h, rgba, 156):
        art += bytes((0, (r * a + 255 * (255 - a)) // 255,
                      (g * a + 255 * (255 - a)) // 255,
                      (b * a + 255 * (255 - a)) // 255))
    out += resource('sART', 128, art)
    # Native-size header art avoids runtime resampling of the large mascot.
    art = struct.pack('>HH', 52, 52)
    for r, g, b, a in scale(w, h, rgba, 52):
        art += bytes((0, (r * a + 221 * (255 - a)) // 255,
                      (g * a + 221 * (255 - a)) // 255,
                      (b * a + 221 * (255 - a)) // 255))
    out += resource('sART', 129, art)
    # Raw resource bytes avoid toolchain-specific BNDL/FREF Rez templates.
    out += resource('ShCk', 0, b'\0')
    out += resource('FREF', 128, b'APPL\0\0\0')
    bundle = b'ShCk' + struct.pack('>HH', 0, 1)
    for kind in (b'ICN#', b'FREF'):
        bundle += kind + struct.pack('>HHH', 0, 0, 128)
    out += resource('BNDL', 128, bundle)
    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_text(out)
    print(f'Generated Finder icons and 156px/52px window artwork: {args.output}')


if __name__ == '__main__':
    main()
