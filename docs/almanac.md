# Almanac architecture

How the Almanac attaches to CrossPoint, and what the port actually changed.

## Integration surface

The Almanac touches **three** upstream files. Everything else is new code under
`src/activities/almanac/` and `src/activities/dictionary/`.

| Upstream file | Change |
| --- | --- |
| `src/activities/ActivityManager.h` | one enum value: `HomeMenuItem::ALMANAC` |
| `src/activities/home/HomeActivity.h` | menu index mappers + `onAlmanacOpen()` |
| `src/activities/home/HomeActivity.cpp` | menu count, one row, one dispatch case, one opener |

Keeping the surface this small is deliberate: it makes rebasing onto upstream
tractable. When upstream moves, those three files are the only merge conflicts
you should ever see.

## Activity tree

```
HomeActivity
└── AlmanacActivity              submenu hub
    ├── DictionaryActivity       "/dictionary.cdb"
    ├── DictionaryActivity       "/gazetteer.cdb"   (same class, different file)
    ├── SkyActivity
    ├── TsumegoActivity
    ├── ChessActivity
    ├── ClockActivity
    └── LocationActivity
```

`DictionaryActivity` takes a path and a title, so the World Factbook is the same
class pointed at a different WCDB file — exactly as the watch's `curRef` worked.
One engine, two modules.

`Location` (NVS-backed, seeded once from an optional `/location.txt`) is the only
shared state: `SkyActivity` reads latitude and longitude from it, and
`TimeSource` reads the standard-time offset and DST rule. Nothing is hardcoded to
a timezone or a city.

## What ported verbatim, and what was rewritten

The rule throughout: **logic ports, presentation is rewritten.** Everything the
watch validated stays validated.

**Ported unchanged**

* `sky_math.h` — pure C, no Arduino dependencies. Compiles here and in the
  desktop harness that was checked against `astropy`.
* `stars.h` — `PROGMEM` / `memcpy_P` are no-ops on the ESP32, so the generated
  catalogue is byte-identical.
* Go engine — flood-fill liberties, capture, suicide, ko (banned off-tree only,
  since some tsumego lines encode threat-free ko), the variation-tree walker,
  history + replay-based undo, solution search, NVS solved bitmap.
* Chess engine — `toSq()` board flip, `chessApply()` with castling / en passant /
  promotion, load / replay / attempt, and the two-phase selection rule.
* WCDB block engine — binary-searched block index, per-block raw DEFLATE, `>`
  synonym redirects, prefix search.

**Rewritten for this device**

* **Storage** — `SPIFFS.open()` → `Storage.open()` / `HalFile`. Data lives on the
  SD card, not internal flash.
* **Decompression** — the ESP32 ROM's `tinfl` → CrossPoint's `InflateReader`
  (uzlib). Raw DEFLATE, so `skipZlibHeader()` is *not* called. Reusing the
  in-tree decompressor keeps the engine portable off the ESP32.
* **Input** — raw GPIO edge/hold/repeat polling → `MappedInputManager` +
  `ButtonNavigator`. List navigation uses `onNext()` / `onPrevious()`, which
  resolve to `NavNext` / `NavPrevious` (side Down + front Right, side Up + front
  Left) and honour the orientation swap.
* **Drawing** — GxEPD2 → `GfxRenderer`. It has no circle primitives, so
  `fillCircle` / `drawCircle` are hand-rolled where needed (sky disk, moon glyph,
  Go stones, chess halos).
* **Layout** — 200×200 → 480×800. The sky disk grows from R=74 to R≈220; Go
  board pitch from 24 px to 56 px; chess squares from 25 px to ~56 px. The chess
  module was originally shelved because twelve piece identities at 25 px was
  illegible — a verdict about that screen, not the code.
* **Cursors** — the watch had four buttons, so its cursors stepped "to the next
  empty point" or "down a row". With a D-pad both boards move in two dimensions.
  The legality rules are untouched; only the navigation changed.
* **Solution playback** — blocking `delay(800)` loops became `millis()`-stepped
  state machines in `loop()`, because an Activity may not block the render lock.
* **Location** — the watch hardcoded Ottawa and the US/Canada DST rule into
  `sky.h` and `sky_math.h`. Both now come from `Location`, with offsets carried
  in *minutes* so quarter-hour zones (+05:45, +12:45) work. Southern-hemisphere
  DST is not implemented; the EU rule is approximated in local time rather than
  at its true 01:00 UTC boundary.

## Two conventions worth knowing

**`GfxRenderer::drawText(fontId, x, y, …)` takes the TOP of the text, not the
baseline** — it adds the ascender internally. Adding it yourself pushes every
string down by one ascender, which manifests as rows overlapping and text
vanishing out of highlight rectangles.

**Act on a button release only if this activity also saw the press.** A child
activity finishes on a Back *or* Confirm event, and the trailing release lands in
the parent's next `loop()`. Pre-arming a lock for "the" pending button guesses
wrong half the time and leaves a stale lock that eats a later, legitimate press
(symptom: menu items needing two clicks). Press/release matching is
self-correcting and needs no flags.

## Memory and storage notes

* No PSRAM; ~327 KB usable RAM. The WCDB reader sizes its decompression buffers
  from the file's own worst-case `rawSize`/`compSize` rather than a hardcoded
  ceiling.
* The Go and chess solved-bitmaps are ~1.5 KB and ~1.2 KB NVS blobs, in separate
  namespaces (`tsumego`, `chess`). They share the NVS partition with CrossPoint's
  own settings. If `putBytes` ever begins failing, suspect NVS pressure rather
  than the game code.
* Bulk data is read from the SD card on demand; nothing large is resident.
