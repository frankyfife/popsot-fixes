# Repairs the empty textures of the HD texture pack (Evgesha.JK, PoP Texture
# Studio) with the same textures from the 4K pack, scaled down to the HD size.
#
# 32 replacements of the HD pack are empty (black or fully transparent), e.g.
# walls and floor of the treasure vault; the 4K pack has them intact (except
# one that is transparent there as well). dx.dll leaves empty textures to the
# game; after this repair the pack shows its own HD versions instead.
#
# Usage:  python repair_hd_pack.py "<game folder>\Evgesha.JK" "<4K pack>.zip | <4K Evgesha.JK>"
#
# Needs Python 3 with Pillow (11.2 or newer, for DXT encoding) and numpy. A
# backup (Evgesha.JK.bak) is written next to the file first, unless it exists.
# The file is changed in place: the payloads of the repaired textures (same
# size, format and mip levels), their SHA-256 in the index and the index hash
# in the header, which the pack checks when it starts.
#
# File layout: 0x90-byte header ("EVGJK1", texture count +0x18, overlay count
# +0x1c, index offset +0x30, overlay index offset +0x38, SHA-256 of texture
# index + overlay index at +0x70), 0x68-byte texture records (key +0, original
# width/height/levels/format +4, replacement width/height/levels/format +0xa,
# payload offset/size +0x18, SHA-256 of the original texture +0x28 and of the
# payload +0x48), 0x60-byte overlay records. Formats: 1 DXT1, 2 DXT5, 3
# A8R8G8B8. Payloads hold all levels; a side is halved per level only while
# it is 9 or more.
import hashlib
import io
import os
import shutil
import struct
import sys
import zipfile

import numpy as np
from PIL import Image

HEADER, RECORD, OVERLAY = 0x90, 0x68, 0x60


def read_index(f):
    f.seek(0)
    head = f.read(HEADER)
    if head[:6] != b"EVGJK1":
        sys.exit("not an Evgesha.JK file")
    count, overlays = struct.unpack_from("<II", head, 0x18)
    idx_off, ov_off = struct.unpack_from("<QQ", head, 0x30)
    f.seek(idx_off)
    index = bytearray(f.read(count * RECORD))
    f.seek(ov_off)
    ovl = f.read(overlays * OVERLAY)
    return bytearray(head), index, ovl, count


def record(index, i):
    r = index[i * RECORD:(i + 1) * RECORD]
    key, = struct.unpack_from("<I", r, 0)
    rw, rh, levels, fmt = struct.unpack_from("<HHBB", r, 0xa)
    off, size = struct.unpack_from("<QQ", r, 0x18)
    return key, rw, rh, levels, fmt, off, size


def level_sizes(w, h, levels):
    out = []
    for lv in range(levels):
        if lv:
            w = w >> 1 if w >= 9 else w
            h = h >> 1 if h >= 9 else h
        out.append((w, h))
    return out


def level_bytes(w, h, fmt):
    if fmt == 3:
        return w * h * 4
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fmt == 1 else 16)


def decode_top(data, w, h, fmt):
    """Top level as an RGBA array."""
    if fmt == 3:
        a = np.frombuffer(data[:w * h * 4], np.uint8).reshape(h, w, 4)
        return a[..., [2, 1, 0, 3]]
    hdr = bytearray(128)
    hdr[0:4] = b"DDS "
    struct.pack_into("<IIIIIII", hdr, 4, 124, 0x1 | 0x2 | 0x4 | 0x1000, h, w, 0, 0, 1)
    struct.pack_into("<II4s", hdr, 76, 32, 4, b"DXT1" if fmt == 1 else b"DXT5")
    struct.pack_into("<I", hdr, 108, 0x1000)
    im = Image.open(io.BytesIO(bytes(hdr) + data[:level_bytes(w, h, fmt)]))
    return np.asarray(im.convert("RGBA"))


def empty(rgba):
    """Fewer than 1 % visible pixels (brightest channel and alpha above 8)."""
    visible = (rgba[..., :3].max(axis=2) > 8) & (rgba[..., 3] > 8)
    return visible.mean() < 0.01


def encode(rgba, w, h, fmt):
    im = Image.fromarray(rgba, "RGBA").resize((w, h), Image.LANCZOS)
    if fmt == 3:
        return np.asarray(im)[..., [2, 1, 0, 3]].tobytes()
    b = io.BytesIO()
    im.save(b, "DDS", pixel_format="DXT1" if fmt == 1 else "DXT5")
    data = b.getvalue()[128:]
    need = level_bytes(w, h, fmt)
    if len(data) != need:
        sys.exit(f"unexpected DXT size {len(data)} for {w}x{h} (want {need})")
    return data


def open_4k(path):
    if path.lower().endswith(".zip"):
        z = zipfile.ZipFile(path)
        name = next(n for n in z.namelist() if n.lower().endswith("evgesha.jk"))
        return z.open(name)
    return open(path, "rb")


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: python repair_hd_pack.py "<game folder>\\Evgesha.JK" "<4K pack>.zip | <4K Evgesha.JK>"')
    hd_path, src_path = sys.argv[1], sys.argv[2]

    with open(hd_path, "rb") as f:
        head, index, ovl, count = read_index(f)
        todo = {}
        for i in range(count):
            key, w, h, levels, fmt, off, size = record(index, i)
            f.seek(off)
            if empty(decode_top(f.read(level_bytes(w, h, fmt)), w, h, fmt)):
                todo[key] = i
    print(f"{len(todo)} empty textures in the HD pack")
    if not todo:
        return

    # 4K pack: read as a stream (it may be inside the zip), in payload order.
    src = open_4k(src_path)
    shead = src.read(HEADER)
    scount, = struct.unpack_from("<I", shead, 0x18)
    sidx_off, = struct.unpack_from("<Q", shead, 0x30)
    src.read(sidx_off - HEADER)
    sindex = src.read(scount * RECORD)
    pos = sidx_off + scount * RECORD
    wanted = []
    for i in range(scount):
        key, w, h, levels, fmt, off, size = record(sindex, i)
        if key in todo:
            wanted.append((off, key, w, h, fmt, size))
    wanted.sort()

    fixes = {}
    for off, key, w, h, fmt, size in wanted:
        while pos < off:
            pos += len(src.read(min(off - pos, 64 << 20)))
        data = src.read(size)
        pos += len(data)
        rgba = decode_top(data, w, h, fmt)
        if empty(rgba):
            print(f"  {key:08X}: empty in the 4K pack as well, left as it is")
            continue
        i = todo[key]
        _, hw, hh, levels, hfmt, hoff, hsize = record(index, i)
        payload = b"".join(encode(rgba, lw, lh, hfmt) for lw, lh in level_sizes(hw, hh, levels))
        if len(payload) != hsize:
            sys.exit(f"{key:08X}: payload size {len(payload)} differs from {hsize}")
        fixes[key] = (i, hoff, payload)
        print(f"  {key:08X}: {w}x{h} -> {hw}x{hh}, {levels} levels", flush=True)
    missing = set(todo) - {k for _, k, *_ in wanted}
    for key in sorted(missing):
        print(f"  {key:08X}: not in the 4K pack")
    if not fixes:
        return

    backup = hd_path + ".bak"
    if not os.path.exists(backup):
        print(f"backup: {backup}", flush=True)
        shutil.copyfile(hd_path, backup)

    with open(hd_path, "r+b") as f:
        for key, (i, off, payload) in fixes.items():
            f.seek(off)
            f.write(payload)
            index[i * RECORD + 0x48:i * RECORD + 0x68] = hashlib.sha256(payload).digest()
        idx_off, ov_off = struct.unpack_from("<QQ", head, 0x30)
        head[0x70:0x90] = hashlib.sha256(bytes(index) + ovl).digest()
        f.seek(idx_off)
        f.write(index)
        f.seek(0)
        f.write(head)
    print(f"{len(fixes)} textures repaired")


if __name__ == "__main__":
    main()
