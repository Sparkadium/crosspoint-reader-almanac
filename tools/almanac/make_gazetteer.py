#!/usr/bin/env python3
"""
make_gazetteer.py — Build gazetteer.cdb (country reference) for WatchyDict.

Data source (no hand-entered facts anywhere):
  CIA World Factbook, JSON conversion (public domain):
  https://github.com/factbook/factbook.json
  Expects the repo extracted as ./factbook.json-master/

Output format: WCDB, identical to dictionary.cdb — reuses the writer
from prepare_dict_compressed.py so the on-watch engine needs no changes.
Keys are lowercase country names with spaces replaced by '-' (the watch
search grid includes '-').

Usage: python3 make_gazetteer.py [--output gazetteer.cdb]
"""
import json, glob, os, re, sys, argparse, html, unicodedata
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prepare_dict_fat import write_cdb as build_compressed_file

def to_ascii(s):
    """Decode HTML entities (&iacute; -> í) then transliterate to plain
    ASCII (í -> i) — the watch font has ASCII glyphs only."""
    s = html.unescape(html.unescape(str(s)))     # twice: handles &amp;iacute;
    s = unicodedata.normalize('NFKD', s)
    return s.encode('ascii', 'ignore').decode('ascii')

def dig(d, *path, default=''):
    for k in path:
        if not isinstance(d, dict) or k not in d:
            return default
        d = d[k]
    return d

def clean(s, maxlen=None):
    s = to_ascii(s)
    s = re.sub(r'<[^>]+>', ' ', s)
    s = re.sub(r'\s+', ' ', s).strip()
    if maxlen and len(s) > maxlen:              # trim at word boundary
        s = s[:maxlen].rsplit(' ', 1)[0]
        while s.count('(') > s.count(')'):       # drop unclosed fragment
            s = s[:s.rfind('(')].rstrip()
        s = s.rstrip(',;: ')
    return s

# ── Display-fit budgeting ─────────────────────────────────────────
# The watch wraps definitions with FreeMono9pt7b (11px fixed advance)
# into 192px -> 17 chars/line, 11 lines/page. Entries must fit 5 pages
# (55 lines). This mirrors drawWrappedText() exactly (conservative).
WRAP_CHARS = 17
MAX_LINES = 54                                   # one line of margin

def wrap_lines(text):
    lines, rem = 0, text.strip()
    while rem:
        fc = min(WRAP_CHARS, len(rem))
        if fc < len(rem):
            ls = rem.rfind(' ', 0, fc)
            if ls > 0: fc = ls + 1
        lines += 1
        rem = rem[fc:].strip()
    return lines

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', default='gazetteer.cdb')
    ap.add_argument('--src', default='factbook.json-master')
    args = ap.parse_args()

    lines, seen = [], set()
    for f in sorted(glob.glob(os.path.join(args.src, '*', '*.json'))):
        try:
            d = json.load(open(f, encoding='utf-8'))
        except Exception:
            continue
        name = dig(d, 'Government', 'Country name',
                   'conventional short form', 'text')
        if not name or name.lower() == 'none':
            name = dig(d, 'Government', 'Country name',
                       'conventional long form', 'text')
        if not name or name.lower() == 'none':
            name = os.path.basename(f)[:-5]
        name = to_ascii(name)
        key = name.lower().strip().replace(' ', '-')
        key = re.sub(r"[^a-z\-']", '', key)[:28]
        if not key or key in seen:
            continue
        parts = []
        cap  = dig(d, 'Government', 'Capital', 'name', 'text')
        pop  = (dig(d, 'People and Society', 'Population', 'total', 'text')
                or dig(d, 'People and Society', 'Population', 'text'))
        area = dig(d, 'Geography', 'Area', 'total', 'text')
        lang = dig(d, 'People and Society', 'Languages', 'Languages', 'text')
        loc  = dig(d, 'Geography', 'Location', 'text')
        bg   = dig(d, 'Introduction', 'Background', 'text')
        if cap:  parts.append(f'Cap: {clean(cap, 36)}')
        if pop:  parts.append(f'Pop: {clean(pop, 36)}')
        if area: parts.append(f'Area: {clean(area, 36)}')
        if lang: parts.append(f'Lang: {clean(lang, 80)}')
        if loc:  parts.append(f'Loc: {clean(loc, 80)}')
        entry = '; '.join(parts)
        # add background one sentence at a time while it still fits
        if bg:
            for sent in re.split(r'(?<=[.!?]) +', clean(bg)):
                cand = (entry + '; ' + sent) if entry else sent
                if wrap_lines(cand) > MAX_LINES:
                    break
                entry = cand
        if not entry:
            continue
        assert wrap_lines(entry) <= MAX_LINES, key
        entry = entry.replace('\t', ' ').replace('\n', ' ')
        lines.append(f'{key}\t{entry}\n')
        seen.add(key)

    lines.sort()
    print(f'{len(lines)} countries/territories')
    build_compressed_file(lines, args.output)

if __name__ == '__main__':
    main()
