#!/usr/bin/env python3
# make_countries.py -- builds /countries.bin: country borders for the Globe's
# reticle -> CIA World Factbook link.
#
# Data source (public domain): Natural Earth 1:110m admin-0 countries,
#   https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_admin_0_countries.geojson
#
# Each country stores its Factbook WCDB key, computed with the SAME transform
# as make_gazetteer.py (lowercase, spaces->hyphens, keep [a-z-'], 28 chars),
# applied to the CIA "conventional short form" name. ALIASES maps Natural
# Earth's (often abbreviated) names onto those CIA names; anything unmapped
# falls back to its own name, and a key the gazetteer does not contain simply
# shows the name with no entry -- graceful, never wrong.
#
# CTY2 format (LE):
#   "CTY2" | u16 countryCount
#   per country: u8 keyLen, key | u8 nameLen, displayName
#                int16 flyLat, flyLon (centidegrees: the capital from Natural
#                    Earth populated places, or the largest ring's centroid
#                    when no capital exists -- e.g. Antarctica)
#                int16 latMin,latMax,lonMin,lonMax (centidegrees bbox)
#                u16 ringCount
#                per ring: u16 pointCount, pointCount x (int16 lat, int16 lon)
#                          (centidegrees)

import json, re, struct, os, sys, urllib.request

SRC = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_admin_0_countries.geojson"
SRC_P = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_populated_places_simple.geojson"

# Natural Earth NAME -> the name whose derived key exists in gazetteer.cdb.
# These are NOT guessed CIA "official" forms: they were audited against the
# key set derived from factbook.json by the EXACT logic in make_gazetteer.py
# (short form, falling back to long form -- which is why the DRC is literally
# "DRC" and Micronesia truncates from its long form).
ALIASES = {
    "United States of America": "United States",
    "Dem. Rep. Congo": "DRC",
    "Congo": "Congo (Brazzaville)",
    "Central African Rep.": "Central African Republic",
    "S. Sudan": "South Sudan",
    "Eq. Guinea": "Equatorial Guinea",
    "W. Sahara": "Western Sahara",
    "Cote d'Ivoire": "Cote d'Ivoire",
    "eSwatini": "Eswatini",
    "Somaliland": "Somalia",
    "Bosnia and Herz.": "Bosnia and Herzegovina",
    "Czechia": "Czechia",
    "North Macedonia": "North Macedonia",
    "Kosovo": "Kosovo",
    "Myanmar": "Burma",
    "North Korea": "North Korea",
    "South Korea": "South Korea",
    "Turkey": "Turkey",
    "Dominican Rep.": "The Dominican",
    "Bahamas": "The Bahamas",
    "Gambia": "The Gambia",
    "Falkland Is.": "Falkland Islands (Islas Malvinas)",
    "Fr. S. Antarctic Lands": "French Southern and Antarctic Lands",
    "Solomon Is.": "Solomon Islands",
    "Timor-Leste": "Timor-Leste",
    "Taiwan": "Taiwan",
    "N. Cyprus": "Cyprus",
    "Palestine": "West Bank",
    "Greenland": "Greenland",
    "New Caledonia": "New Caledonia",
    "Puerto Rico": "Puerto Rico",
    "Vanuatu": "Vanuatu",
    "Brunei": "Brunei",
    "Laos": "Laos",
    "Vietnam": "Vietnam",
    "Russia": "Russia",
    "Iran": "Iran",
    "Syria": "Syria",
    "Venezuela": "Venezuela",
    "Bolivia": "Bolivia",
    "Tanzania": "Tanzania",
    "Moldova": "Moldova",
    "United Kingdom": "United Kingdom",
    "Antarctica": "Antarctica",
    "St-Martin": "Saint Martin",
    "Cabo Verde": "Cabo Verde",
    "Sao Tome and Principe": "Sao Tome and Principe",
}

# populated-places adm0name -> the NE polygon NAME it belongs to. Without
# this bridge these four match no polygon and fly to a CENTROID, not their
# capital.
PLACE_TO_NE = {
    "Congo (Brazzaville)": "Congo",
    "Congo (Kinshasa)": "Dem. Rep. Congo",
    "The Gambia": "Gambia",
    "Guinea Bissau": "Guinea-Bissau",
}

# adm0name -> CIA conventional short form, for capitals that have NO polygon
# at 110m (microstates and island nations). Everything not listed maps by its
# own name.
PLACE_CIA = {
    "Vatican": "Holy See (Vatican City)",
    "Cape Verde": "Cabo Verde",
}

def to_ascii(t):  # same idea as make_gazetteer.py: no '?' in display names
    M = {"\u00e0":"a","\u00e1":"a","\u00e2":"a","\u00e3":"a","\u00e4":"a","\u00e7":"c",
         "\u00e8":"e","\u00e9":"e","\u00ea":"e","\u00eb":"e","\u00ed":"i","\u00ee":"i",
         "\u00f1":"n","\u00f3":"o","\u00f4":"o","\u00f6":"o","\u00fa":"u","\u00fc":"u",
         "\u00c5":"A","\u00e5":"a","\u2019":"'"}
    return "".join(M.get(c, c) for c in t)

def cia_key(name):  # EXACT transform from make_gazetteer.py
    key = name.lower().strip().replace(' ', '-')
    return re.sub(r"[^a-z\-']", '', key)[:28]

def main():
    if not os.path.exists("admin0.geojson"):
        print("downloading", SRC)
        urllib.request.urlretrieve(SRC, "admin0.geojson")
    if not os.path.exists("places.geojson"):
        print("downloading", SRC_P)
        urllib.request.urlretrieve(SRC_P, "places.geojson")
    gj = json.load(open("admin0.geojson"))

    # admin-0 capitals: adm0name -> (lat, lon)
    capitals, alt = {}, {}
    for f in json.load(open("places.geojson"))["features"]:
        p = f["properties"]
        fc = (p.get("featurecla") or "").lower().strip()
        # EXACT class match: substring matching also catches "Admin-1 capital"
        # (state capitals -- Denver, Osaka) and flies to the wrong city.
        if fc != "admin-0 capital" and fc != "admin-0 capital alt":
            continue
        lon, lat = f["geometry"]["coordinates"]
        # "alt" marks historic/secondary capitals (Kyoto, The Hague...) --
        # only ever a fallback.
        (alt if fc.endswith("alt") else capitals).setdefault(p.get("adm0name", ""), (lat, lon))
    for k, v in alt.items():
        capitals.setdefault(k, v)
    # remap bridged adm0names onto their polygon NAMEs
    for a, nename in PLACE_TO_NE.items():
        if a in capitals:
            capitals[nename] = capitals.pop(a)
    consumed = set()

    out = []
    for f in gj["features"]:
        ne = f["properties"]["NAME"]
        cia = ALIASES.get(ne, ne)
        key = cia_key(to_ascii(cia))
        disp = to_ascii(ne)[:40]
        g = f["geometry"]
        polys = g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]]
        rings = []
        la0, la1, lo0, lo1 = 90.0, -90.0, 180.0, -180.0
        for poly in polys:
            ring = poly[0]  # outer ring only: holes are other countries,
                            # which win by being tested too (last match wins
                            # on the device via smaller-bbox preference)
            pts = []
            for lon, lat in ring:
                lat = max(-89.99, min(89.99, lat))
                lon = max(-179.99, min(179.99, lon))
                pts.append((int(round(lat * 100)), int(round(lon * 100))))
                la0, la1 = min(la0, lat), max(la1, lat)
                lo0, lo1 = min(lo0, lon), max(lo1, lon)
            if len(pts) >= 4:
                rings.append(pts)
        if rings:
            # fly-to point: the capital when NE has one for this admin-0 name,
            # else the centroid of the largest ring
            cap = None
            for lookup in (f["properties"].get("ADMIN", ""), ne):
                if lookup in capitals:
                    cap = capitals[lookup]
                    consumed.add(lookup)
                    break
            if cap is None:
                big = max(rings, key=len)
                cap = (sum(p[0] for p in big) / len(big) / 100.0,
                       sum(p[1] for p in big) / len(big) / 100.0)
            out.append((key, disp, cap, (la0, la1, lo0, lo1), rings))

    # capitals with no polygon at this scale: microstates and island nations
    # join as point entries (fly-to + a tiny bbox for reticle resolution; no
    # rings, so no borders -- they are sub-pixel at globe scale anyway)
    have_keys = {k for k, *_ in out}
    added = 0
    for adm0, cap in capitals.items():
        if adm0 in consumed:
            continue
        cia = PLACE_CIA.get(adm0, adm0)
        key = cia_key(to_ascii(cia))
        if key in have_keys:
            continue
        disp = to_ascii(adm0)[:40]
        la, lo = cap
        bb = (max(-89.9, la - 0.15), min(89.9, la + 0.15), max(-179.9, lo - 0.15), min(179.9, lo + 0.15))
        out.append((key, disp, cap, bb, []))
        added += 1
    print(f"added {added} capital-only microstates")

    out.sort(key=lambda t: t[1].lower())  # alphabetical for the Find list

    npts = 0
    with open("countries.bin", "wb") as f:
        f.write(b"CTY2" + struct.pack("<H", len(out)))
        for key, disp, cap, bb, rings in out:
            kb, db = key.encode(), disp.encode("ascii", "replace")
            f.write(struct.pack("<B", len(kb)) + kb)
            f.write(struct.pack("<B", len(db)) + db)
            f.write(struct.pack("<hh", int(round(cap[0]*100)), int(round(cap[1]*100))))
            f.write(struct.pack("<hhhh", int(bb[0]*100), int(bb[1]*100), int(bb[2]*100), int(bb[3]*100)))
            f.write(struct.pack("<H", len(rings)))
            for pts in rings:
                f.write(struct.pack("<H", len(pts)))
                for la, lo in pts:
                    f.write(struct.pack("<hh", la, lo))
                npts += len(pts)
    print(f"countries.bin: {len(out)} countries, {npts} points, {os.path.getsize('countries.bin')} bytes")

if __name__ == "__main__":
    main()
