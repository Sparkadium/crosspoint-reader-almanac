#!/usr/bin/env python3
# make_texture.py -- equirectangular texture -> TEX1 binary for the Almanac's
# textured globe modes (Earth photo mode, Moon photo mode, Planetarium).
#
# Sources: Solar System Scope textures (CC BY 4.0, NASA-derived imagery)
#   https://www.solarsystemscope.com/textures/
# These are imagery, not measured data: clouds are a static snapshot and
# sunspots are baked in. Orientation math elsewhere in the suite remains
# host-verified; texture files are attributed art. See README.
#
# Pipeline (identical to globe_tuner.html, where these curves were chosen):
#   RGB -> Rec.709 luminance -> v**gamma -> (v-0.5)*contrast + 0.5 + bright
#   -> clamp -> quantize to 8-bit rows.
# Dithering happens ON DEVICE (Floyd-Steinberg, serpentine) so partial
# renders and future modes can re-dither without re-converting.
#
# Format TEX1 (little-endian):
#   0  "TEX1"
#   4  uint16 width
#   6  uint16 height
#   8  width*height bytes, 8-bit luminance, row-major, row 0 = lat +90,
#      col 0 = lon -180 (matches globe.bin conventions)
#
# Usage:
#   python make_texture.py 2k_earth_daymap.jpg earth_day
#   python make_texture.py --all      (expects the 2k_*.jpg files beside it)
#
# Output goes to tex_<name>.bin; copy to SD card root. (The tex_ prefix
# keeps the Moon texture clear of /moon.bin, the IAU features file.)
import sys, os, struct
from PIL import Image
import numpy as np

TW, TH = 1024, 512

# Approved curves -- chosen by eye in globe_tuner.html, 2026-07-15.
# Device dither mode for all bodies: Floyd-Steinberg.
SETTINGS = {
    "moon":         {"gamma": 0.80, "bright": 0.00, "contrast": 3.00},
    "sun":          {"gamma": 0.80, "bright": 0.00, "contrast": 3.00},
    "earth_day":    {"gamma": 0.80, "bright": 0.00, "contrast": 2.40},
    "earth_clouds": {"gamma": 0.80, "bright": 0.00, "contrast": 2.40},
    "earth_night":  {"gamma": 1.21, "bright": 0.50, "contrast": 2.20},
    # Planetarium bodies (untuned yet: neutral until run through the tuner)
    "default":      {"gamma": 1.00, "bright": 0.00, "contrast": 1.00},
}

DEFAULT_FILES = {
    "earth_day":    "2k_earth_daymap.jpg",
    "earth_night":  "2k_earth_nightmap.jpg",
    "earth_clouds": "2k_earth_clouds.jpg",
    "moon":         "2k_moon.jpg",
    "sun":          "2k_sun.jpg",
    # Planetarium bodies -- same Solar System Scope 2K set; tune curves in
    # globe_tuner.html and add entries to SETTINGS when you have favourites.
    "mercury":      "2k_mercury.jpg",
    "venus":        "2k_venus_atmosphere.jpg",
    "mars":         "2k_mars.jpg",
    "jupiter":      "2k_jupiter.jpg",
    "saturn":       "2k_saturn.jpg",
    "uranus":       "2k_uranus.jpg",
    "neptune":      "2k_neptune.jpg",
}

def convert(src, name):
    s = SETTINGS.get(name, SETTINGS["default"])
    im = Image.open(src).convert("L").resize((TW, TH), Image.LANCZOS)
    v = np.asarray(im, dtype=np.float64) / 255.0
    v = np.power(np.clip(v, 0, 1), s["gamma"])
    v = (v - 0.5) * s["contrast"] + 0.5 + s["bright"]
    v = np.clip(v, 0, 1)
    data = (v * 255.0 + 0.5).astype(np.uint8)
    out = "tex_" + name + ".bin"
    with open(out, "wb") as f:
        f.write(b"TEX1")
        f.write(struct.pack("<HH", TW, TH))
        f.write(data.tobytes())
    print("%s: %dx%d, %d bytes  (gamma %.2f  bright %+.2f  contrast %.2f)" % (
        out, TW, TH, os.path.getsize(out), s["gamma"], s["bright"], s["contrast"]))

if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--all":
        for name, fn in DEFAULT_FILES.items():
            if os.path.exists(fn):
                convert(fn, name)
            else:
                print("skip %s: %s not found" % (name, fn))
    elif len(sys.argv) == 3:
        convert(sys.argv[1], sys.argv[2])
    else:
        print(__doc__ or "usage: make_texture.py <image> <name> | --all")
        sys.exit(1)
