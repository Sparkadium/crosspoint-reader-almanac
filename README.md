# CrossInk Almanac

A reference library, atlas, planetarium, and puzzle collection for the
Xteink X4/X3 e-readers, built as a fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
(CrossInk variant). Everything runs offline from the SD card on an
ESP32-C3 with ~320 KB of RAM, and everything on screen is computed or
sourced — nothing hand-drawn, nothing invented.

## Modules

**Dictionary** — 69,000+ word offline dictionary. Type-ahead search,
prefix browsing, synonym redirects, random word, adjustable text size.

**Wikipedia** — Simple English Wikipedia, same engine. The block-compressed
WCDB format streams from the SD card with binary search over an on-card
index, so corpus size is limited by the card, not by RAM.

**World Factbook** — the CIA World Factbook as a gazetteer: geography,
government, history, and statistics for every country and territory.

**Globe** — a spinning orthographic Earth under a fixed crosshair.
Live day/night terminator with civil-twilight band, country borders,
country identification under the reticle with one-press Factbook entry,
find-a-country (205 countries, flies to the capital), three zoom levels,
and a full-bleed space mode with starfield. Everything is toggleable down
to a bare planet.

**Moon** — the Moon as it faces you tonight. The home view is the real
sub-Earth point (optical libration included), the terminator matches the
current phase, and ~400 named features from the IAU/USGS Gazetteer of
Planetary Nomenclature appear with zoom-based level of detail: maria as
dotted regions, craters as rim circles at true scale. The reticle names
the nearest feature.

**Sky Chart** — all-sky star chart for your location: 904 stars (Yale
Bright Star Catalogue, V ≤ 4.5) with constellation lines, moon phase,
sunrise/sunset, and time travel in 30-minute or 1-day steps. One gesture
hides all chrome for a bare sky.

**Calculator** — graphing calculator with a purpose-built one-layer math
keypad, six expression slots, and Y=-style function plotting.

**Chess** — Lichess puzzle trainer with rating bands and native vector
piece sprites.

**Tsumego** — 11,800+ Go life-and-death problems with tree-walking
solution verification and progress tracking.

**Clock / Location** — set the time and your coordinates/UTC offset/DST
rule; every sky module derives from these. If the clock is unset, the sky
modules show a clearly-labelled fixed fallback moment instead of a dead
screen.

## SD card setup

See [SD_CARD.md](SD_CARD.md). Short version: eight data files go in the
card root; each module degrades gracefully (and says which file it wants)
if its file is absent. All files are downloadable from the Releases page
or regenerable with the scripts in `tools/`.

## Building

PlatformIO. The device firmware is the `xlarge` env:

```
pio run -e xlarge --target upload
```

Font variants: `tiny` ships the smaller reading sizes, `xlarge` the three
largest (16/18/20 px). UI code adapts to whichever sizes a variant
actually carries.

X3 hardware clock: the `xlarge` env builds with `ALMANAC_USE_FREEINK_RTC`,
so an X3's battery-backed RTC keeps time across power-off. The X4 has no
clock chip; it reports that honestly and asks for the time after deep
sleep.

## Data provenance

A strict rule applies throughout: all data comes from authoritative,
scriptable public sources. No hand-entered records, no generated data.
Every data file is rebuilt from source by a script in `tools/`:

| Data | Source | License |
|---|---|---|
| Coastlines, borders, capitals | [Natural Earth](https://www.naturalearthdata.com) 1:110m | Public domain |
| Country facts | [CIA World Factbook](https://github.com/factbook/factbook.json) | Public domain |
| Lunar features | [USGS/IAU Gazetteer of Planetary Nomenclature](https://planetarynames.wr.usgs.gov) | Public domain |
| Stars | Yale Bright Star Catalogue (FK5) | Free use |
| Chess puzzles | [Lichess puzzle database](https://database.lichess.org/#puzzles) | CC0 |
| Tsumego problems | [sanderland/tsumego](https://github.com/sanderland/tsumego) | MIT |
| Dictionary/Wikipedia | Wiktionary / Simple English Wikipedia | CC BY-SA |

## Verification

Every mathematical component ships with a host-side test harness
(`tools/almanac/`) comparing the exact C++ that runs on the device against
an independent reference. Current receipts:

- Sun and sky positions: verified against astropy
- Day/night terminator geometry: 2.85M pixel checks against brute force,
  0 mismatches
- Country hit-testing and capital coordinates: 25 cases including
  enclaves (Lesotho), microstates inside larger polygons (Singapore,
  Vatican), and open ocean
- Moon orientation: 501 dates over 2020–2030 against JPL DE421's
  integrated lunar librations, worst error 0.11°
- Calculator engine: 451 expressions against Python

Anyone can rerun these: each harness is a single `g++` command plus, where
a reference is needed, a Python script that generates it from public
ephemerides or the standard library.

## Devices

Xteink X4 (ESP32-C3, 480×800 e-ink) is the primary target; the X3 is
supported through CrossPoint's runtime board detection. E-ink niceties
include dithered night shading, disc-local ghost scrubbing instead of
full-panel refreshes, and fast-refresh navigation throughout.

## Credits

Built by [Sparkadium](https://github.com/Sparkadium) in collaboration with
Claude (Anthropic). The division of labor, honestly stated: ideas,
direction, hardware testing, and stubbornness were human; much of the code
was written by the AI; all of it was verified against independent
references before it shipped, per the receipts above.

Forked from CrossPoint Reader / CrossInk — thanks to their authors for a
clean, hackable e-reader firmware.
