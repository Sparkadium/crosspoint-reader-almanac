#!/usr/bin/env python3
"""
gen_sprites40.py — 40x40 1-bit chess sprites for the Xteink X4 (and any board
with squares of ~46px or more).

The original 20x20 masks were drawn for the Watchy's 25px squares. On the X4 the
squares are ~56px, and the firmware was scaling those masks 2x, which turns every
edge into a 2px staircase. Doubling pixels cannot add detail; the fix is to draw
at the target size.

Method: each piece is drawn as vector primitives at 8x supersample (320x320),
downsampled with a box filter, then thresholded at 50%. That produces smooth,
consistent silhouettes with proper anti-alias-free 1-bit edges, and it is
reproducible -- no hand-placed pixels to get subtly wrong.

  fill mask    -> black pieces (solid silhouette)
  outline mask -> white pieces (silhouette minus its own erosion)

Emits sprites40.h with 40-bit rows packed into uint64_t, and a preview PNG
showing every piece on light and dark squares so the result can be inspected
before it ever reaches the device.

Usage: python3 gen_sprites40.py
"""
from PIL import Image, ImageDraw, ImageChops, ImageFilter

S = 40           # sprite size in device pixels
SS = 8           # supersample factor
N = S * SS       # working canvas


def px(v):
    """Normalised [0,1] -> supersampled pixels."""
    return v * N


def ellipse(d, cx, cy, rx, ry):
    d.ellipse([px(cx - rx), px(cy - ry), px(cx + rx), px(cy + ry)], fill=255)


def poly(d, pts):
    d.polygon([(px(x), px(y)) for x, y in pts], fill=255)


def rect(d, x0, y0, x1, y1):
    d.rectangle([px(x0), px(y0), px(x1), px(y1)], fill=255)


def cut_poly(d, pts):
    d.polygon([(px(x), px(y)) for x, y in pts], fill=0)


def cut_ellipse(d, cx, cy, rx, ry):
    d.ellipse([px(cx - rx), px(cy - ry), px(cx + rx), px(cy + ry)], fill=0)


def base(d, wide=0.86):
    """Foot and plinth shared by every piece."""
    half = wide / 2
    poly(d, [(0.5 - half, 0.965), (0.5 + half, 0.965),
             (0.5 + half - 0.05, 0.885), (0.5 - half + 0.05, 0.885)])
    rect(d, 0.30, 0.845, 0.70, 0.895)


def pawn(d):
    ellipse(d, 0.5, 0.245, 0.135, 0.135)          # head
    rect(d, 0.40, 0.365, 0.60, 0.405)             # collar
    poly(d, [(0.425, 0.405), (0.575, 0.405),
             (0.655, 0.845), (0.345, 0.845)])     # tapered body
    base(d, 0.74)


def rook(d):
    # crenellations
    for x0 in (0.20, 0.425, 0.65):
        rect(d, x0, 0.135, x0 + 0.15, 0.285)
    rect(d, 0.20, 0.255, 0.80, 0.345)             # battlement band
    rect(d, 0.265, 0.345, 0.735, 0.405)           # cornice
    poly(d, [(0.315, 0.405), (0.685, 0.405),
             (0.735, 0.845), (0.265, 0.845)])     # body, slight flare
    base(d)


def bishop(d):
    ellipse(d, 0.5, 0.115, 0.045, 0.045)          # finial
    poly(d, [(0.5, 0.145), (0.645, 0.40), (0.355, 0.40)])   # mitre
    ellipse(d, 0.5, 0.315, 0.145, 0.115)          # mitre body
    # The diagonal slit is what tells a bishop from a pawn at a glance.
    cut_poly(d, [(0.515, 0.175), (0.575, 0.175), (0.635, 0.325), (0.575, 0.325)])
    rect(d, 0.355, 0.395, 0.645, 0.445)           # collar
    poly(d, [(0.395, 0.445), (0.605, 0.445),
             (0.695, 0.845), (0.305, 0.845)])
    base(d)


def knight(d):
    # Horse in profile, facing left. The muzzle, jaw and eye do the work: a
    # knight that reads as a blob is worse than no knight.
    poly(d, [
        (0.520, 0.105),   # crown, between the ears
        (0.615, 0.140),
        (0.690, 0.235),
        (0.740, 0.360),
        (0.755, 0.470),
        (0.720, 0.590),   # back of the neck
        (0.715, 0.720),
        (0.735, 0.845),   # base, right
        (0.265, 0.845),   # base, left
        (0.285, 0.715),
        (0.300, 0.605),   # chest
        (0.245, 0.520),   # jaw
        (0.155, 0.470),   # muzzle tip
        (0.170, 0.395),
        (0.255, 0.355),   # bridge of the nose
        (0.345, 0.330),
        (0.420, 0.255),   # forehead
        (0.455, 0.165),
    ])
    poly(d, [(0.470, 0.130), (0.545, 0.030), (0.585, 0.150)])   # ear
    cut_poly(d, [(0.300, 0.395), (0.360, 0.360), (0.375, 0.415), (0.315, 0.445)])  # cheek notch
    cut_ellipse(d, 0.405, 0.330, 0.038, 0.038)                  # eye
    base(d)


def queen(d):
    # five crown points, each capped with a bead
    pts = [(0.175, 0.235), (0.335, 0.185), (0.5, 0.165), (0.665, 0.185), (0.825, 0.235)]
    for cx, cy in pts:
        ellipse(d, cx, cy, 0.055, 0.055)
    poly(d, [(0.155, 0.255), (0.845, 0.255), (0.735, 0.445), (0.265, 0.445)])
    rect(d, 0.265, 0.435, 0.735, 0.485)           # crown band
    poly(d, [(0.325, 0.485), (0.675, 0.485),
             (0.735, 0.845), (0.265, 0.845)])
    base(d)


def king(d):
    rect(d, 0.455, 0.055, 0.545, 0.235)           # cross, vertical
    rect(d, 0.375, 0.115, 0.625, 0.185)           # cross, horizontal
    ellipse(d, 0.5, 0.315, 0.185, 0.115)          # crown dome
    rect(d, 0.265, 0.375, 0.735, 0.445)           # crown band
    poly(d, [(0.315, 0.445), (0.685, 0.445),
             (0.745, 0.845), (0.255, 0.845)])
    base(d)


PIECES = [("P", pawn), ("N", knight), ("B", bishop), ("R", rook), ("Q", queen), ("K", king)]


def render(fn):
    img = Image.new("L", (N, N), 0)
    fn(ImageDraw.Draw(img))
    small = img.resize((S, S), Image.BOX)              # box filter = area average
    return small.point(lambda v: 255 if v >= 128 else 0)  # 50% threshold


def outline(mask):
    # A 1px outline looks fragile at 56px squares; erode by 2 for a 2px stroke.
    eroded = mask.filter(ImageFilter.MinFilter(5))
    return ImageChops.subtract(mask, eroded)


def halo(mask):
    """The piece's own silhouette, dilated 3px.

    The watch drew a white DISC behind a piece on a dark square so it would not
    merge with the stipple. At 20px in a 25px square a disc covered everything.
    At 40px in a 56px square it does not: crenellations, queen beads, the king's
    cross and every base corner fall outside it. A disc big enough to cover them
    would erase the whole square and with it the checker pattern. A dilated
    silhouette hugs the piece, covers all of it, and leaves stipple in the
    corners."""
    return mask.filter(ImageFilter.MaxFilter(7))


def grid(img):
    p = img.load()
    return [[1 if p[x, y] else 0 for x in range(S)] for y in range(S)]


def preview(fills, lines, halos, path, sq=56, scale=5):
    """Four rows: black piece on light/dark, white piece on light/dark. The dark
    square uses the same 3px stipple and white halo as the firmware."""
    img = Image.new("RGB", (6 * sq, 4 * sq), "white")
    p = img.load()
    off = (sq - S) // 2
    halo_r2 = int(sq * 11 / 25) ** 2
    step = 3

    for i, (name, _) in enumerate(PIECES):
        for row in range(4):
            cx, cy = i * sq, row * sq
            dark = row in (1, 3)
            if dark:
                for y in range(1, sq, step):
                    for x in range(1, sq, step):
                        p[cx + x, cy + y] = (0, 0, 0)
                hg = grid(halos[name])
                for y in range(S):
                    for x in range(S):
                        if hg[y][x]:
                            p[cx + x + off, cy + y + off] = (255, 255, 255)
            g = grid(fills[name] if row < 2 else lines[name])
            for y in range(S):
                for x in range(S):
                    if g[y][x]:
                        p[cx + x + off, cy + y + off] = (0, 0, 0)

    img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    img.save(path)


def emit(fills, lines, halos, path):
    out = [f"// generated by gen_sprites40.py: {S}x{S} masks, 1 bit per pixel",
           "// Vector-drawn at 8x supersample, box-downsampled, thresholded at 50%.",
           "// fill masks (black pieces), outline masks (white pieces).",
           "// Rows are 40 bits wide, so uint64_t -- NOT uint32_t like the 20px set.",
           "#pragma once",
           "#include <cstdint>",
           f"#define SPR40 {S}"]
    for kind, src in (("FILL", fills), ("LINE", lines), ("HALO", halos)):
        for name, _ in PIECES:
            g = grid(src[name])
            rows = []
            for y in range(S):
                v = 0
                for x in range(S):
                    if g[y][x]:
                        v |= 1 << x
                rows.append(f"0x{v:010X}ULL")
            out.append(f"const uint64_t SPR40_{kind}_{name}[SPR40] = " + "{" + ",".join(rows) + "};")
    order = ",".join(f"SPR40_FILL_{n}" for n, _ in PIECES)
    out.append(f"const uint64_t* const SPR40_FILL[6] = {{{order}}};")
    order = ",".join(f"SPR40_LINE_{n}" for n, _ in PIECES)
    out.append(f"const uint64_t* const SPR40_LINE[6] = {{{order}}};")
    order = ",".join(f"SPR40_HALO_{n}" for n, _ in PIECES)
    out.append(f"const uint64_t* const SPR40_HALO[6] = {{{order}}};")
    open(path, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    fills = {n: render(f) for n, f in PIECES}
    lines = {n: outline(fills[n]) for n, _ in PIECES}
    halos = {n: halo(fills[n]) for n, _ in PIECES}
    preview(fills, lines, halos, "sprites40_preview.png")
    emit(fills, lines, halos, "sprites40.h")

    for n, _ in PIECES:
        g = grid(fills[n])
        ink = sum(sum(r) for r in g)
        cols = [x for x in range(S) if any(g[y][x] for y in range(S))]
        rows = [y for y in range(S) if any(g[y][x] for x in range(S))]
        print(f"  {n}: {ink:4d} px ink, bbox x[{min(cols)},{max(cols)}] y[{min(rows)},{max(rows)}]")
    print("wrote sprites40.h and sprites40_preview.png")
