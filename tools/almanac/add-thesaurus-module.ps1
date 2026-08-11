# add-thesaurus-module.ps1
#
# Adds a Thesaurus entry to the Almanac hub. The thesaurus needs no new module:
# DictionaryActivity already takes (cdbPath, title), so this is a fourth
# instance of the same WCDB engine reading /thesaurus.cdb -- the same trick
# Wikipedia and the World Factbook already use.
#
# Anchor-based and idempotent: each edit checks for its own marker first, so
# re-running is a no-op. Run from the repo root.
#
# Usage:  pwsh -File tools/almanac/add-thesaurus-module.ps1
#         pwsh -File tools/almanac/add-thesaurus-module.ps1 -Revert

param(
    [string]$RepoRoot = ".",
    [switch]$Revert
)

$ErrorActionPreference = "Stop"

$HeaderPath = Join-Path $RepoRoot "src/activities/almanac/AlmanacActivity.h"
$CppPath    = Join-Path $RepoRoot "src/activities/almanac/AlmanacActivity.cpp"

foreach ($p in @($HeaderPath, $CppPath)) {
    if (-not (Test-Path $p)) { throw "Not found: $p  (run from the repo root, or pass -RepoRoot)" }
}

# Read/write without touching the encoding: UTF8 with no BOM, LF preserved.
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Read-Src  ([string]$p) { [System.IO.File]::ReadAllText($p, $Utf8NoBom) }
function Write-Src ([string]$p, [string]$t) { [System.IO.File]::WriteAllText($p, $t, $Utf8NoBom) }

$emDash = [string][char]0x2014   # the file's comment style; kept out of this script's bytes
$changes = 0

function Apply-Edit {
    param([string]$Path, [string]$Label, [string]$Find, [string]$Replace, [switch]$Regex)

    $text = Read-Src $Path

    if ($Revert) {
        $tmp = $Find; $Find = $Replace; $Replace = $tmp
    }

    if ($Regex) {
        $hits = ([regex]$Find).Matches($text).Count
    } else {
        $hits = ([regex]::Matches($text, [regex]::Escape($Find))).Count
    }

    if ($hits -eq 0) {
        # Already in the target state?
        $done = if ($Regex) { ([regex]$Replace).IsMatch($text) } else { $text.Contains($Replace) }
        if ($done) { Write-Host "  skip   $Label (already applied)" -ForegroundColor DarkGray; return }
        throw "anchor not found for '$Label' in $Path -- the file has drifted; patch by hand"
    }
    if ($hits -gt 1) {
        throw "anchor for '$Label' matched $hits times in $Path -- refusing to patch ambiguously"
    }

    if ($Regex) {
        $text = [regex]::Replace($text, $Find, $Replace)
    } else {
        $text = $text.Replace($Find, $Replace)
    }
    Write-Src $Path $text
    Write-Host "  ok     $Label" -ForegroundColor Green
    $script:changes++
}

Write-Host ""
Write-Host ($(if ($Revert) { "Removing" } else { "Adding" }) + " Thesaurus module") -ForegroundColor Cyan
Write-Host "AlmanacActivity.h"

# 1. Doc block at the top of the header, so the module list stays truthful.
Apply-Edit -Path $HeaderPath -Label "header doc comment" -Regex `
    -Find   "(?m)^(//   Dictionary .*)$" `
    -Replace ('$1' + "`n//   Thesaurus       " + $emDash + " same engine, /thesaurus.cdb")

# 2. Item enum. Position is load-bearing: it indexes NAMES[] and blurbs_[].
Apply-Edit -Path $HeaderPath -Label "Item enum" `
    -Find    "enum Item { DICTIONARY = 0, WIKIPEDIA," `
    -Replace "enum Item { DICTIONARY = 0, THESAURUS, WIKIPEDIA,"

Write-Host "AlmanacActivity.cpp"

# 3. NAMES[] -- must stay in enum order. clang-format will reflow the wrap.
Apply-Edit -Path $CppPath -Label "NAMES[] row label" `
    -Find    'const char* NAMES[] = {"Dictionary", "Wikipedia",' `
    -Replace 'const char* NAMES[] = {"Dictionary", "Thesaurus", "Wikipedia",'

# 4. Subtitle. Reads the count out of the WCDB header rather than hardcoding
#    one, exactly as the other three data modules do.
Apply-Edit -Path $CppPath -Label "refreshBlurbs() entry" `
    -Find    '  blurbs_[SKY] = "stars, moon phase, sun times";' `
    -Replace @'
  const uint32_t senses = wcdbEntryCount("/thesaurus.cdb");
  blurbs_[THESAURUS] = senses ? withCommas(senses) + " headwords"
                              : "thesaurus.cdb not on SD card";
  blurbs_[SKY] = "stars, moon phase, sun times";
'@.TrimEnd("`r", "`n")

# 5. open() switch case. std::make_unique matches the surrounding cases; the
#    activity is constructed on open and freed on exit, so this adds no
#    resident allocation.
Apply-Edit -Path $CppPath -Label "open() switch case" `
    -Find    "    case FACTBOOK:" `
    -Replace @'
    case THESAURUS:
      status_.clear();
      startActivityForResult(
          std::make_unique<DictionaryActivity>(renderer, mappedInput, "/thesaurus.cdb", "Thesaurus"),
          onReturn);
      break;
    case FACTBOOK:
'@.TrimEnd("`r", "`n")

Write-Host ""
if ($changes -eq 0) {
    Write-Host "No changes needed." -ForegroundColor DarkGray
} else {
    Write-Host "$changes edit(s) applied. Next:" -ForegroundColor Cyan
    Write-Host "  ./bin/clang-format-fix     # NAMES[] wrap will be reflowed"
    Write-Host "  pio run -e xlarge"
}
Write-Host ""
