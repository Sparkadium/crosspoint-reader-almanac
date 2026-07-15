#!/usr/bin/env python3
# make_globe.py -- builds /globe.bin for the Globe module.
#
# Data source (public domain): Natural Earth 1:110m coastlines, via the
# project's official GitHub mirror:
#   https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_coastline.geojson
# Natural Earth explicitly places all its data in the public domain
# (https://www.naturalearthdata.com/about/terms-of-use/). Nothing here is
# hand-entered: rerun this script and the file regenerates from source.
#
# GLB1 format:
#   "GLB1" | u16 lineCount
#   per line: u8 flags (0 = coastline, solid; 1 = graticule, dotted;
#                        2 = country border, dashed, toggleable on device)
#             u16 pointCount
#             pointCount x (int16 x, int16 y, int16 z)   unit vector * 32000
#
# Points are stored as PRE-COMPUTED 3D unit vectors so the device never does
# trig per point: rotating the globe is nine multiplications per point.
# Segments longer than ~2 degrees are subdivided so chords do not visibly cut
# through the sphere. World frame: x -> (lat 0, lon 0), y -> (0, 90E), z -> N.

import json, math, struct, sys, urllib.request, os

SRC = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_coastline.geojson"
SRC_B = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_admin_0_boundary_lines_land.geojson"
SCALE = 32000
MAX_STEP_DEG = 2.0

def unit(lat, lon):
    la, lo = math.radians(lat), math.radians(lon)
    return (math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la))

def slerp(a, b, t):
    d = max(-1.0, min(1.0, a[0]*b[0] + a[1]*b[1] + a[2]*b[2]))
    ang = math.acos(d)
    if ang < 1e-9: return a
    sa, sb = math.sin((1 - t) * ang) / math.sin(ang), math.sin(t * ang) / math.sin(ang)
    v = tuple(sa * a[i] + sb * b[i] for i in range(3))
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v)

def densify(pts):
    out = [pts[0]]
    for p in pts[1:]:
        a = out[-1]
        d = max(-1.0, min(1.0, a[0]*p[0] + a[1]*p[1] + a[2]*p[2]))
        steps = max(1, int(math.degrees(math.acos(d)) / MAX_STEP_DEG))
        for s in range(1, steps + 1):
            out.append(slerp(a, p, s / steps))
    return out

def main():
    if not os.path.exists("coast.geojson"):
        print("downloading", SRC)
        urllib.request.urlretrieve(SRC, "coast.geojson")
    if not os.path.exists("borders.geojson"):
        print("downloading", SRC_B)
        urllib.request.urlretrieve(SRC_B, "borders.geojson")
    gj = json.load(open("coast.geojson"))

    lines = []  # (flags, [unit vectors])
    for f in gj["features"]:
        g = f["geometry"]
        for line in (g["coordinates"] if g["type"] == "MultiLineString" else [g["coordinates"]]):
            pts = densify([unit(la, lo) for lo, la in line])
            # int16 pointCount: split absurdly long lines defensively
            while len(pts) > 60000:
                lines.append((0, pts[:60000])); pts = pts[59999:]
            lines.append((0, pts))

    # country borders (land boundaries only, already deduplicated by NE)
    for f in json.load(open("borders.geojson"))["features"]:
        g = f["geometry"]
        for line in (g["coordinates"] if g["type"] == "MultiLineString" else [g["coordinates"]]):
            pts = densify([unit(la, lo) for lo, la in line])
            lines.append((2, pts))

    # graticule, every 30 degrees, point spacing ~2 degrees (drawn as dots)
    for glat in range(-60, 61, 30):
        lines.append((1, [unit(glat, lo) for lo in range(-180, 181, 2)]))
    for glon in range(0, 331, 30):
        lines.append((1, [unit(la, glon) for la in range(-88, 89, 2)]))

    npts = sum(len(p) for _, p in lines)
    with open("globe.bin", "wb") as f:
        f.write(b"GLB1" + struct.pack("<H", len(lines)))
        for flags, pts in lines:
            f.write(struct.pack("<BH", flags, len(pts)))
            for v in pts:
                f.write(struct.pack("<hhh", *(int(round(c * SCALE)) for c in v)))
    print(f"globe.bin: {len(lines)} lines, {npts} points, {os.path.getsize('globe.bin')} bytes")

if __name__ == "__main__":
    main()
