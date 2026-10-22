"""Framebuffer decoding and pointer-finding helpers for the guest harness.

Stdlib only. A capture is rejected rather than guessed at: a PPM that is
malformed, truncated or carries trailing bytes raises ImageError, so callers
never analyse half a frame.
"""
import struct
import zlib
from collections import defaultdict

MAX_DIMENSION = 8192


class ImageError(Exception):
    pass


def parse_ppm(data):
    """Decode a binary P6 PPM; return (width, height, rgb bytes)."""
    if data[:2] != b"P6":
        raise ImageError("not a P6 PPM")
    pos = 2
    fields = []
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        try:
            fields.append(int(data[start:pos]))
        except ValueError:
            raise ImageError("malformed PPM header") from None
    pos += 1  # the single whitespace byte that ends the header
    w, h, maxval = fields
    if not (0 < w <= MAX_DIMENSION and 0 < h <= MAX_DIMENSION):
        raise ImageError("implausible PPM size %dx%d" % (w, h))
    if maxval != 255:
        raise ImageError("unsupported PPM maxval %d" % maxval)
    want = w * h * 3
    have = len(data) - pos
    if have < want:
        raise ImageError("truncated PPM: %d of %d pixel bytes" % (have, want))
    if have > want:
        raise ImageError("PPM has %d trailing bytes" % (have - want))
    return w, h, data[pos:pos + want]


def read_ppm(path):
    with open(path, "rb") as f:
        return parse_ppm(f.read())


def encode_ppm(w, h, rgb):
    return b"P6\n%d %d\n255\n" % (w, h) + bytes(rgb)


def encode_png(w, h, rgb):
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))


def dark_mask(w, h, rgb, lum_thresh=110, region=None):
    """Pixels dark in all channels; region is (x0, y0, x1, y1) or None."""
    x0, y0, x1, y1 = region or (0, 0, w, h)
    pts = set()
    for y in range(max(0, y0), min(h, y1)):
        row = y * w * 3
        for x in range(max(0, x0), min(w, x1)):
            i = row + x * 3
            if rgb[i] < lum_thresh and rgb[i + 1] < lum_thresh and rgb[i + 2] < lum_thresh:
                pts.add((x, y))
    return pts


def diff_boxes(w, h, rgb_a, rgb_b, thresh=48):
    """Bounding boxes of the connected components of changed pixels."""
    changed = set()
    for i in range(0, w * h * 3, 3):
        if (abs(rgb_a[i] - rgb_b[i]) > thresh
                or abs(rgb_a[i + 1] - rgb_b[i + 1]) > thresh
                or abs(rgb_a[i + 2] - rgb_b[i + 2]) > thresh):
            p = i // 3
            changed.add((p % w, p // w))
    boxes = []
    while changed:
        seed = changed.pop()
        stack = [seed]
        minx = maxx = seed[0]
        miny = maxy = seed[1]
        while stack:
            cx, cy = stack.pop()
            for nx in (cx - 1, cx, cx + 1):
                for ny in (cy - 1, cy, cy + 1):
                    if (nx, ny) in changed:
                        changed.discard((nx, ny))
                        stack.append((nx, ny))
                        minx, maxx = min(minx, nx), max(maxx, nx)
                        miny, maxy = min(miny, ny), max(maxy, ny)
        boxes.append((minx, miny, maxx, maxy))
    return boxes


def find_template(w, h, dark_pts, tmpl, min_dark=0.8, max_results=400):
    """Masked template match on the arrow's dark outline.

    tmpl: offsets that must be dark. Returns [(x, y, fraction matched)],
    best first, in a deterministic order.
    """
    n = len(tmpl)
    scores = defaultdict(int)
    probes = [tmpl[0], tmpl[n // 2], tmpl[-1], tmpl[n // 3]]
    for (dx, dy) in probes:
        for (px, py) in dark_pts:
            scores[(px - dx, py - dy)] += 1
    results = []
    for (x, y), seed in scores.items():
        if seed < 2 or x < 0 or y < 0 or x >= w or y >= h:
            continue
        matched = sum(1 for (dx, dy) in tmpl if (x + dx, y + dy) in dark_pts)
        score = matched / n
        if score < min_dark:
            continue
        results.append((x, y, score))
    results.sort(key=lambda r: (-r[2], r[1], r[0]))
    return results[:max_results]
