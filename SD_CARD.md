# SD card setup

The Almanac's modules read their data from the SD card root. The firmware
works with any subset — a module whose file is missing says so on its menu
line and does nothing worse — but for the full suite you need all of these
in the root of the card:

| File | Module | Source | Built by |
|---|---|---|---|
| `dictionary.cdb` | Dictionary | Wiktionary-derived word list | `tools/prepare_dict_fat.py` |
| `wikipedia.cdb` | Wikipedia | Simple English Wikipedia | wiki build script |
| `gazetteer.cdb` | World Factbook | CIA World Factbook (factbook.json) | `tools/almanac/make_gazetteer.py` |
| `globe.bin` | Globe | Natural Earth 110m (public domain) | `tools/almanac/make_globe.py` |
| `countries.bin` | Globe (Facts/Find) | Natural Earth + Factbook naming | `tools/almanac/make_countries.py` |
| `moon.bin` | Moon | IAU/USGS planetary nomenclature | `tools/almanac/make_moon.py` |
| `chess.bin` | Chess | Lichess puzzle database (CC0) | chess build script |
| `problems.bin` | Tsumego | sanderland/tsumego (MIT) | tsumego build script |

Optional: `location.txt` in the root seeds your latitude/longitude/UTC offset
on first boot (see the comment block in `src/.../Location.h` for the format).
After that, set it on-device under Almanac -> Location.

Every file is either downloadable from this repo's Releases page or
regenerable from public sources with the scripts above — nothing is
hand-entered.

Works on both X4 and X3. X3 note: to keep the hardware clock across sleep,
build with the RTC flag (see `TimeSource.h`); without it the X3 behaves like
the X4 and asks for the time after deep sleep.
