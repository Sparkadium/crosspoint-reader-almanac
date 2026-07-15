#!/usr/bin/env python3
# make_moon.py -- builds /moon.bin: named lunar features for the Moon globe.
#
# Data source (public domain, US government work): the USGS/IAU Gazetteer of
# Planetary Nomenclature, https://planetarynames.wr.usgs.gov
#
# RUN THIS ON YOUR OWN MACHINE. It first tries to download the Moon feature
# CSV directly; if the endpoint shape has changed, download it manually
# (Advanced Search -> Target: Moon -> output CSV, all approved features) and:
#     python make_moon.py SearchResults.csv
# Nothing is hand-entered either way: rerun and the file regenerates.
#
# MON1 format (LE):
#   "MON1" | u16 featureCount
#   per feature: u8 rank (0 = mare-class, 1 = crater >= 100 km,
#                         2 = crater >= 60 km, 3 = everything else kept)
#                u8 nameLen, name (ASCII)
#                int16 lat, int16 lon   (centidegrees, lon east +, +-180)
#                u16 diameterKm
#
# Kept: all mare-class features (Mare/Oceanus/Sinus/Lacus/Palus), mountains
# (Mons/Montes), valleys (Vallis), and craters >= 30 km, capped to the 400
# largest overall so the device file stays small and the disc stays legible.

import csv, io, os, re, struct, sys, tempfile, unicodedata, urllib.request, zipfile

# Endpoint variants tried in order; the site's query interface has changed
# shape over the years. If none yields CSV, download manually (see below).
URLS = [
    # The real GIS asset: an S3 bucket, generated nightly, linked from
    # planetarynames.wr.usgs.gov/GIS_Downloads. Static and separate from the
    # site's search app, so it works even when the site itself is down.
    "https://asc-planetarynames-data.s3.us-west-2.amazonaws.com/MOON_nomenclature_center_pts.zip",
    "https://planetarynames.wr.usgs.gov/SearchResults?Target=16_Moon&displayType=CSV",
    "https://planetarynames.wr.usgs.gov/SearchResults?target=MOON&displayType=CSV",
]
MANUAL = """
Could not fetch a CSV automatically. Manual route (one minute):
  1. Open https://planetarynames.wr.usgs.gov  ->  Advanced Search
  2. Target: Moon. Leave other filters alone. Search.
  3. On the results page choose the CSV output/export option and save it.
  4. Rerun:  python make_moon.py SearchResults.csv
"""

MARE = ("Mare", "Oceanus", "Sinus", "Lacus", "Palus")
KEEP = MARE + ("Mons", "Montes", "Vallis", "Crater")
MIN_CRATER_KM = 30
CAP = 400

def to_ascii(t):
    return unicodedata.normalize("NFKD", t).encode("ascii", "ignore").decode()

def rows_from_shapefile(data):
    """Yield dict rows from a nomenclature shapefile ZIP (or bare .shp path).
    Field names are matched loosely; prints them on mismatch so a fix is a
    one-line paste."""
    try:
        import shapefile  # pyshp
    except ImportError:
        raise SystemExit("shapefile input needs pyshp:  pip install pyshp")
    if isinstance(data, bytes):
        tmp = tempfile.mkdtemp()
        zipfile.ZipFile(io.BytesIO(data)).extractall(tmp)
        shp = [os.path.join(tmp, f) for f in os.listdir(tmp) if f.lower().endswith(".shp")]
        if not shp:
            raise SystemExit("no .shp inside the ZIP")
        r = shapefile.Reader(shp[0])
    else:
        r = shapefile.Reader(data)
    fields = [f[0] for f in r.fields[1:]]
    def find(*subs):
        for f in fields:
            fl = f.lower()
            if all(s in fl for s in subs):
                return f
        raise SystemExit(f"shapefile field {subs} not found -- fields are: {fields}")
    F_NAME = find("name")
    F_DIAM = find("diam")
    F_LAT = find("lat")
    F_LON = find("lon")
    F_TYPE = find("type")
    for rec in r.records():
        d = dict(zip(fields, rec))
        yield {"feature_name": d[F_NAME], "diameter": d[F_DIAM],
               "center_latitude": d[F_LAT], "center_longitude": d[F_LON],
               "feature_type": d[F_TYPE]}

def main():
    raw = None
    shp_rows = None
    if len(sys.argv) > 1:
        p = sys.argv[1]
        if p.lower().endswith((".zip", ".shp")):
            shp_rows = rows_from_shapefile(open(p, "rb").read()
                                           if p.lower().endswith(".zip") else p)
        else:
            raw = open(p, encoding="utf-8-sig").read()
            if raw.lstrip()[:1] == "<":
                raise SystemExit("that file is an HTML page, not a CSV." + MANUAL)
    else:
        for url in URLS:
            print("trying", url)
            try:
                data = urllib.request.urlopen(url, timeout=60).read()
            except Exception as e:
                print("  ", e)
                continue
            if data[:2] == b"PK":  # the shapefile ZIP
                shp_rows = rows_from_shapefile(data)
                break
            body = data.decode("utf-8-sig", "replace")
            head = body.lstrip()[:200].lower()
            if head.startswith("<") or "<html" in head:
                print("   got an HTML page, not CSV")
                continue
            raw = body
            break
        if raw is None and shp_rows is None:
            raise SystemExit(MANUAL)

    if shp_rows is not None:
        rows = list(shp_rows)
        cols = {k: k for k in rows[0]} if rows else {}
        rdr = rows
    else:
        rdr = csv.DictReader(io.StringIO(raw))
        cols = {c.lower().strip(): c for c in rdr.fieldnames or []}
    def col(*names):
        for n in names:
            if n in cols: return cols[n]
        raise SystemExit(f"CSV missing column: {names} -- got {list(cols)}")
    C_NAME = col("feature_name", "name", "clean_feature_name")
    C_DIAM = col("diameter", "diameter_km")
    C_LAT = col("center_latitude", "latitude")
    C_LON = col("center_longitude", "longitude")
    C_TYPE = col("feature_type", "type")

    feats = []
    for row in rdr:
        try:
            typ = (row[C_TYPE] or "").split(",")[0].strip()
            base = typ.split()[0] if typ else ""
            if base not in KEEP: continue
            diam = float(row[C_DIAM] or 0)
            if base == "Crater" and diam < MIN_CRATER_KM: continue
            lat = float(row[C_LAT]); lon = float(row[C_LON])
            if lon > 180: lon -= 360
            name = to_ascii(row[C_NAME]).strip()[:40]
            if not name: continue
            rank = 0 if base in MARE else (1 if diam >= 100 else (2 if diam >= 60 else 3))
            feats.append((rank, name, lat, lon, min(int(diam), 65535)))
        except (ValueError, KeyError):
            continue

    feats.sort(key=lambda f: (f[0], -f[4]))  # mare-class first, then biggest
    feats = feats[:CAP]
    with open("moon.bin", "wb") as f:
        f.write(b"MON1" + struct.pack("<H", len(feats)))
        for rank, name, lat, lon, diam in feats:
            nb = name.encode()
            f.write(struct.pack("<BB", rank, len(nb)) + nb)
            f.write(struct.pack("<hhH", int(round(lat * 100)), int(round(lon * 100)), diam))
    print(f"moon.bin: {len(feats)} features")

if __name__ == "__main__":
    main()
