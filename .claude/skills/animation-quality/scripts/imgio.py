"""Minimal stdlib image readers for QA scripts (no Pillow/numpy required).

Supports:
  * PNG: bit depth 8 for gray/RGB/gray+alpha/RGBA, bit depth 1/2/4/8 for palette,
    tRNS transparency, non-interlaced only.
  * GIF: all frames, global/local color tables, transparency, disposal 0-2
    (3 is treated as 1), non-interlaced and interlaced.

An image is returned as Image(w, h, px) where px is a flat list of (r, g, b, a) tuples.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

MAX_PIXELS = 64 * 1024 * 1024  # refuse absurd dimensions (decompression bombs)


@dataclass
class Image:
    w: int
    h: int
    px: list  # flat list of (r, g, b, a)

    def get(self, x: int, y: int):
        return self.px[y * self.w + x]

    def crop(self, x0: int, y0: int, w: int, h: int) -> "Image":
        out = []
        for y in range(y0, y0 + h):
            row = y * self.w
            out.extend(self.px[row + x0: row + x0 + w])
        return Image(w, h, out)


def _check_dims(w: int, h: int) -> None:
    if w <= 0 or h <= 0 or w * h > MAX_PIXELS:
        raise ValueError(f"refusing image of size {w}x{h}")


# ----------------------------------------------------------------------------- PNG
def read_png(path: str) -> Image:
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos = 8
    idat = []
    palette = []
    trns = None
    w = h = depth = ctype = interlace = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype_ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype_ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
            _check_dims(w, h)
        elif ctype_ == b"PLTE":
            palette = [tuple(chunk[i:i + 3]) for i in range(0, len(chunk), 3)]
        elif ctype_ == b"tRNS":
            trns = chunk
        elif ctype_ == b"IDAT":
            idat.append(chunk)
        elif ctype_ == b"IEND":
            break
    if interlace:
        raise ValueError(f"{path}: interlaced PNG not supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    if ctype != 3 and depth != 8:
        raise ValueError(f"{path}: only 8-bit PNG supported for color type {ctype}")
    raw = zlib.decompress(b"".join(idat))
    bits_pp = channels * depth
    stride = (w * bits_pp + 7) // 8
    bpp = max(1, bits_pp // 8)
    prev = bytearray(stride)
    rows = []
    i = 0
    for _ in range(h):
        ft = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ft == 1:
                line[x] = (line[x] + a) & 0xFF
            elif ft == 2:
                line[x] = (line[x] + b) & 0xFF
            elif ft == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        rows.append(line)
        prev = line
    px = []
    for line in rows:
        if ctype == 6:
            px.extend(tuple(line[k:k + 4]) for k in range(0, w * 4, 4))
        elif ctype == 2:
            tr = struct.unpack(">HHH", trns) if trns and len(trns) == 6 else None
            for k in range(0, w * 3, 3):
                r, g, b = line[k], line[k + 1], line[k + 2]
                a = 0 if tr and (r, g, b) == tuple(v & 0xFF for v in tr) else 255
                px.append((r, g, b, a))
        elif ctype == 0:
            tg = struct.unpack(">H", trns)[0] & 0xFF if trns and len(trns) == 2 else None
            px.extend((v, v, v, 0 if v == tg else 255) for v in line[:w])
        elif ctype == 4:
            px.extend((line[k], line[k], line[k], line[k + 1]) for k in range(0, w * 2, 2))
        elif ctype == 3:
            per_byte = 8 // depth
            mask = (1 << depth) - 1
            for xx in range(w):
                byte = line[xx // per_byte]
                shift = (per_byte - 1 - xx % per_byte) * depth
                idx = (byte >> shift) & mask
                r, g, b = palette[idx] if idx < len(palette) else (0, 0, 0)
                a = trns[idx] if trns and idx < len(trns) else 255
                px.append((r, g, b, a))
    return Image(w, h, px)


# ----------------------------------------------------------------------------- GIF
def _lzw_decode(data: bytes, min_code: int, npix: int) -> list:
    clear = 1 << min_code
    eoi = clear + 1
    size = min_code + 1
    table = [[i] for i in range(clear)] + [[], []]
    out = []
    prev = None
    bitpos = 0
    nbits = len(data) * 8
    while bitpos + size <= nbits and len(out) < npix:
        code = 0
        for k in range(size):
            if data[(bitpos + k) >> 3] & (1 << ((bitpos + k) & 7)):
                code |= 1 << k
        bitpos += size
        if code == clear:
            size = min_code + 1
            table = [[i] for i in range(clear)] + [[], []]
            prev = None
            continue
        if code == eoi:
            break
        if code < len(table):
            entry = table[code]
            if prev is not None:
                table.append(prev + [entry[0]])
        elif prev is not None:
            entry = prev + [prev[0]]
            table.append(entry)
        else:
            break
        out.extend(entry)
        prev = entry
        if len(table) == (1 << size) and size < 12:
            size += 1
    return out


def read_gif(path: str) -> list:
    with open(path, "rb") as f:
        d = f.read()
    if d[:3] != b"GIF":
        raise ValueError(f"{path}: not a GIF")
    w, h, flags, _bg, _ = struct.unpack("<HHBBB", d[6:13])
    _check_dims(w, h)
    pos = 13
    gct = []
    if flags & 0x80:
        n = 2 << (flags & 7)
        gct = [tuple(d[pos + i * 3:pos + i * 3 + 3]) for i in range(n)]
        pos += 3 * n
    canvas = [(0, 0, 0, 0)] * (w * h)
    frames = []
    transp = None
    disposal = 0
    while pos < len(d):
        b = d[pos]
        pos += 1
        if b == 0x3B:
            break
        if b == 0x21:
            label = d[pos]
            pos += 1
            if label == 0xF9:
                blk = d[pos + 1:pos + 1 + d[pos]]
                disposal = (blk[0] >> 2) & 7
                transp = blk[3] if blk[0] & 1 else None
            while d[pos]:
                pos += d[pos] + 1
            pos += 1
        elif b == 0x2C:
            fx, fy, fw, fh, fl = struct.unpack("<HHHHB", d[pos:pos + 9])
            pos += 9
            ct = gct
            if fl & 0x80:
                n = 2 << (fl & 7)
                ct = [tuple(d[pos + i * 3:pos + i * 3 + 3]) for i in range(n)]
                pos += 3 * n
            min_code = d[pos]
            pos += 1
            sub = bytearray()
            while d[pos]:
                sub += d[pos + 1:pos + 1 + d[pos]]
                pos += d[pos] + 1
            pos += 1
            idx = _lzw_decode(bytes(sub), min_code, fw * fh)
            rows = list(range(fh))
            if fl & 0x40:  # interlaced
                rows = list(range(0, fh, 8)) + list(range(4, fh, 8)) + list(range(2, fh, 4)) + list(range(1, fh, 2))
            before = list(canvas)
            for r_i, yy in enumerate(rows):
                for xx in range(fw):
                    k = r_i * fw + xx
                    if k >= len(idx):
                        break
                    ci = idx[k]
                    if ci == transp or ci >= len(ct):
                        continue
                    cx, cy = fx + xx, fy + yy
                    if cx < w and cy < h:
                        r, g, bb = ct[ci]
                        canvas[cy * w + cx] = (r, g, bb, 255)
            frames.append(Image(w, h, list(canvas)))
            if disposal == 2:
                for yy in range(fy, min(h, fy + fh)):
                    for xx in range(fx, min(w, fx + fw)):
                        canvas[yy * w + xx] = (0, 0, 0, 0)
            elif disposal == 3:
                canvas = before
            transp = None
            disposal = 0
        else:
            break
    return frames


def read_any(path: str):
    """Return a list of frames for GIFs, or [Image] for PNGs."""
    low = path.lower()
    if low.endswith(".gif"):
        return read_gif(path)
    return [read_png(path)]
