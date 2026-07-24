#!/usr/bin/env python3
# tex_ref.py -- independent integer references for tex_test.cpp.
#   python tex_ref.py earth_day.bin earth_clouds.bin
# Writes ref_resident.bin (the 4-bit resident buffer, day SCREEN clouds)
# and ref_fs.bin (Floyd-Steinberg of a synthetic ramp, identical semantics).
import sys, struct
import numpy as np

SRC_W, SRC_H = 1024, 512
RES_W, RES_H = 512, 256

def load_tex1(path):
    d = open(path, "rb").read()
    assert d[:4] == b"TEX1"
    w, h = struct.unpack("<HH", d[4:8])
    assert (w, h) == (SRC_W, SRC_H)
    return np.frombuffer(d[8:], np.uint8).reshape(SRC_H, SRC_W).astype(np.int64)

def make_resident(day_path, clouds_path, out_path):
    a = load_tex1(day_path)
    b = load_tex1(clouds_path)
    s = 255 - ((255 - a) * (255 - b) + 127) // 255          # screen, rounded
    box = (s[0::2, 0::2] + s[0::2, 1::2] + s[1::2, 0::2] + s[1::2, 1::2] + 2) >> 2
    q = (box * 15 + 127) // 255                             # 0..15, rounded
    packed = bytearray(RES_W * RES_H // 2)
    flat = q.reshape(-1)
    for i in range(0, flat.size, 2):
        packed[i >> 1] = (int(flat[i]) << 4) | int(flat[i + 1])
    open(out_path, "wb").write(bytes(packed))
    print("ref_resident.bin:", len(packed), "bytes")

def make_fs_ref(out_path):
    w = h = 64
    gray = [[((x * 4 + y * 2) % 16) * 17 for x in range(w)] for y in range(h)]
    out = [[0] * w for _ in range(h)]
    cur = [0] * (w + 2)
    nxt = [0] * (w + 2)
    for y in range(h):
        ltr = (y % 2 == 0)
        xs = 1 if ltr else -1
        rng = range(w) if ltr else range(w - 1, -1, -1)
        carry = 0
        for x in rng:
            want = gray[y][x] + cur[x + 1] + carry
            outv = 255 if want >= 128 else 0
            e = want - outv
            out[y][x] = outv
            carry = (e * 7) >> 4
            nxt[x + 1 - xs] += (e * 3) >> 4
            nxt[x + 1] += (e * 5) >> 4
            nxt[x + 1 + xs] += (e * 1) >> 4
        cur, nxt = nxt, cur
        nxt = [0] * (w + 2)
    open(out_path, "wb").write(bytes(bytearray(v for row in out for v in row)))
    print("ref_fs.bin:", w * h, "bytes")

if __name__ == "__main__":
    make_resident(sys.argv[1], sys.argv[2], "ref_resident.bin")
    make_fs_ref("ref_fs.bin")
