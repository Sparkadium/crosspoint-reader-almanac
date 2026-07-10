# CrossPoint Almanac

An offline **Palm-Pilot-style PDA** firmware for the [Xteink X4](https://github.com/open-x4-epaper),
built as a fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).

It keeps CrossPoint's excellent EPUB reader intact and adds an **Almanac** — a
suite of offline reference tools and puzzle trainers ported from
[WatchyAlmanac](https://github.com/sparkadium), a Watchy V2 firmware:

| Module | What it is |
| --- | --- |
| **Dictionary** | 69,457 words with full definitions, compressed-block search |
| **World Factbook** | 261 countries and territories from the CIA World Factbook |
| **Sky Chart** | All-sky star chart, constellation figures, moon phase, sunrise/sunset |
| **Tsumego** | 11,814 Go life-and-death problems with a solved-progress bitmap |
| **Chess** | 9,977 Lichess puzzles across ten rating bands |
| **Clock** | Displays and sets the wall clock |
| **Location** | Latitude, longitude, UTC offset and DST rule |

---

## This is a divergent fork, not a contribution branch

Upstream CrossPoint's [`SCOPE.md`](SCOPE.md) is explicit:

> **Out-of-Scope** — *Interactive Apps: No Notepads, Calculators, or Games. This is a reader, not a PDA.*

This fork is exactly that: a reader **and** a PDA, with two games. That is a
deliberate departure from upstream's mission, not an oversight. **Do not open
pull requests against upstream with these features** — they will be declined on
scope, correctly.

Upstream is MIT licensed, which permits this fork. All upstream code retains its
original copyright (see [`LICENSE`](LICENSE)). Bug fixes to *reader* code that
are genuinely upstream-relevant should still be sent upstream, on their own
branch, without the Almanac.

---

## Installing

### 1. Build and flash

Requires [PlatformIO](https://platformio.org/) (VS Code extension or `pio` CLI).

```sh
git clone --recursive https://github.com/<you>/crosspoint-almanac
cd crosspoint-almanac
pio run                    # compile
pio run --target upload    # flash the X4
pio device monitor         # serial log
```

If you have flashed this device before and hit a boot loop reporting
`partition 0 invalid magic number 0xffff`, see [Flashing notes](#flashing-notes).

### 2. Put the data on the SD card

The Almanac reads its content from the **root of the microSD card** — not from a
folder. Any file you leave out simply reports "not on SD card"; the other modules
keep working.

Two are prebuilt in [`data/`](data/) and can be copied straight over:

| File | What | Licence |
| --- | --- | --- |
| `gazetteer.cdb` | CIA World Factbook, 261 countries | public domain |
| `chess.bin` | 9,977 Lichess puzzles, ten rating bands | CC0 |

The rest you build yourself, because they are large or their licences do not
permit me to redistribute them:

| File | Build with | Needs |
| --- | --- | --- |
| `dictionary.cdb` | `python3 prepare_dict_fat.py` | downloads Wordset |
| `wikipedia.cdb` | `python3 make_wikipedia.py <dump>.xml.bz2` | `pip install mwparserfromhell`, a 350 MB dump |
| `problems.bin` | `python3 tsumego_pack.py pack problems.bin sgf/` | an SGF collection |

Full recipes, formats, and licence notes:
**[`tools/almanac/README.md`](tools/almanac/README.md)**.

The scripts run **once, by hand** — they are not part of the firmware build.
Nothing in `pio run` depends on them; they produce SD-card content, and two of
them download hundreds of megabytes.

The Sky Chart needs no data file — its star catalogue is compiled into the
firmware. It does need to know **where you are**: set your coordinates in
**Almanac → Location**, or drop an optional `/location.txt` on the card to seed
them once:

```ini
lat = 45.3475     # degrees, north positive
lon = -75.7566    # degrees, EAST positive
utc = -300        # standard-time offset in minutes (-300 = UTC-5:00)
dst = na          # none | na | eu
```

The file is read only when no location has been saved yet; after that the
on-device editor owns the value.

## Controls

Open **Almanac** from the home screen. Everywhere: **Back** leaves, **Confirm**
selects, the **D-pad** moves.

| Module | Confirm | Hold Confirm | Back | Hold Back |
| --- | --- | --- | --- | --- |
| Dictionary / Factbook | Search | Random entry | Home | — |
| Sky Chart | Back to now | — | Home | — |
| Tsumego | Place stone | Pass | Undo, then exit | Menu |
| Chess | Select, then play | — | Deselect → undo → exit | Menu |
| Clock | Set / save | — | Cancel, then exit | — |
| Location | Save | — | Discard and exit | — |

In the Dictionary and Factbook, **Left/Right** walk entries alphabetically and
**Up/Down** page a long definition.

In Sky Chart, **Left/Right** step time by 30 minutes and **Up/Down** by a day.

The Tsumego and Chess menus (hold Back) offer: next unsolved, random, browse,
**type a problem number**, problem sets / rating bands, show solution, restart.
Progress is stored in NVS and survives reboots.

---

## The X4 has no real-time clock

CrossPoint's `HalClock` drives a DS3231 and returns early unless the device is an
X3 — the X4 has no RTC chip, which is why upstream hides every clock setting on
this device. Upstream's `SCOPE.md` notes that the ESP32-C3's internal RTC
*drifts significantly during deep sleep*.

The **Clock** module therefore sets the ESP32 system clock directly with
`settimeofday()`. Consequences, stated plainly:

* the time is **lost on a full power-off or battery death**;
* across deep sleep it persists but **drifts** — expect to correct it;
* the **Sky Chart depends on it**. Set the clock before trusting the sky.

Daylight saving is computed from the rule you pick in **Location**. The
North-America and Europe rules are implemented; **southern-hemisphere DST is
not** — choose `none` and set the offset for the season.

This is the same tradeoff as a PCF8563 with no backup cell. Wiring up NTP sync
would fix the drift, at the cost of the connectivity that upstream deliberately
avoids.

---

## Flashing notes

Some X4 units ship with a **Puya** flash chip (manufacturer `0x85`, device
`0x2018`) that will not memory-map-read a partition table written in `dio` mode.
Symptom: the firmware uploads fine, then boot-loops with
`partition 0 invalid magic number 0xffff`.

`platformio.ini` sets `board_build.flash_mode = dio`. On an affected unit,
override it in a local, un-committed `platformio.local.ini` (upstream already
sources this file, and `.gitignore` covers `*.local*`):

```ini
[base]
board_build.flash_mode = qio
board_build.f_flash = 40000000L
```

`dout` prevents USB enumeration entirely. Do not use it.

---

## Attribution

* **CrossPoint Reader** — MIT, © 2025 Dave Allie and contributors. The reader,
  Activity framework, `GfxRenderer`, fonts, HAL, and everything else this fork
  did not write.
* **freeink-sdk / open-x4-epaper** — MIT. Hardware bring-up for the X4.
* **WatchyAlmanac** — the Almanac modules originate here; the Go engine, the
  chess move-application, the WCDB block format, and `sky_math.h` are ported
  essentially verbatim.
* Data sources and their licences are documented separately in
  [`docs/almanac-data-provenance.md`](docs/almanac-data-provenance.md). **No data
  in this project was hand-entered or model-generated.**
