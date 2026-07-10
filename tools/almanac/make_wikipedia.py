#!/usr/bin/env python3
"""
make_wikipedia.py — Build wikipedia.cdb (Simple English Wikipedia) for the
CrossPoint Almanac.

Data source (no hand-entered or model-generated text anywhere):
  Simple English Wikipedia, official Wikimedia XML dump:
  https://dumps.wikimedia.org/simplewiki/latest/simplewiki-latest-pages-articles.xml.bz2

  Wikipedia text is CC BY-SA 4.0. Attribution and share-alike apply to anything
  you redistribute. The .cdb is a derived artifact; build it locally rather than
  committing it. See docs/almanac-data-provenance.md.

Output format: WCDB — identical to dictionary.cdb and gazetteer.cdb, so the
on-device engine needs no changes. Keys are lowercased titles with spaces
replaced by '-' (the device search grid and keyboard both include '-').
Each entry stores the properly-capitalised title, then the lead section.

Only the LEAD SECTION is kept, capped at --max-chars. Reasons:
  - the reader shows plain 1-bit text: no images, infoboxes, tables or links
  - a single entry must stay well under BLOCK_SIZE, or the line-aligned block
    splitter cannot place it
  - lead sections are the part worth having on a pocket device

Requires: pip install mwparserfromhell

Usage:
  python3 make_wikipedia.py simplewiki-latest-pages-articles.xml.bz2
  python3 make_wikipedia.py dump.xml.bz2 --max-chars 2400 --output wikipedia.cdb
  python3 make_wikipedia.py --verify wikipedia.cdb [key]
"""
import argparse
import bz2
import html
import os
import re
import struct
import sys
import unicodedata
import xml.etree.ElementTree as ET
import zlib

BLOCK_SIZE = 32768  # matches prepare_dict_compressed.py; blocks stay <= this

# --------------------------------------------------------------------------
# WCDB writer — same layout as prepare_dict_fat.py::write_cdb.
#   "WCDB" u32 blockCount u32 totalEntries
#   blockCount x (char[32] firstWord, u32 offset, u32 compSize, u32 rawSize)
#   raw-DEFLATE blocks (wbits -15) of "key\ttext\n" records, globally sorted
# --------------------------------------------------------------------------


def split_blocks(raw_text, block_size):
    """Line-aligned blocks that never exceed block_size."""
    blocks, i, n = [], 0, len(raw_text)
    while i < n:
        end = min(i + block_size, n)
        if end < n:
            nl = raw_text.rfind(b"\n", i, end)
            if nl <= i:  # a single line longer than block_size
                nl = raw_text.find(b"\n", end)
                end = n if nl < 0 else nl + 1
            else:
                end = nl + 1
        blocks.append(raw_text[i:end])
        i = end
    return blocks


def write_cdb(lines, output_path):
    raw_text = "".join(lines).encode("utf-8")
    blocks_raw = split_blocks(raw_text, BLOCK_SIZE)
    assert all(len(b) <= BLOCK_SIZE for b in blocks_raw), "block overflow"

    blocks_c = []
    for b in blocks_raw:
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        blocks_c.append(c.compress(b) + c.flush())

    first_words = [b.split(b"\n")[0].split(b"\t")[0].decode() for b in blocks_raw]
    bc = len(blocks_c)
    data_start = 12 + bc * 44
    offs, cur = [], data_start
    for cb in blocks_c:
        offs.append(cur)
        cur += len(cb)

    with open(output_path, "wb") as f:
        f.write(b"WCDB")
        f.write(struct.pack("<I", bc))
        f.write(struct.pack("<I", len(lines)))
        for i in range(bc):
            wb = first_words[i].encode()[:31]
            f.write(wb + b"\0" * (32 - len(wb)))
            f.write(struct.pack("<III", offs[i], len(blocks_c[i]), len(blocks_raw[i])))
        for cb in blocks_c:
            f.write(cb)

    print(f"{len(lines)} articles, {bc} blocks, {os.path.getsize(output_path):,} bytes")
    print(f"index on card: {bc * 44:,} bytes (never loaded into RAM)")


# --------------------------------------------------------------------------
# Wikitext -> plain ASCII
# --------------------------------------------------------------------------

def to_ascii(s):
    """Decode HTML entities, then transliterate to plain ASCII: the device font
    has ASCII glyphs only."""
    s = html.unescape(html.unescape(str(s)))
    s = unicodedata.normalize("NFKD", s)
    return s.encode("ascii", "ignore").decode("ascii")


_HEADING = re.compile(r"^\s*==", re.M)


def lead_section(wikitext):
    m = _HEADING.search(wikitext)
    return wikitext[: m.start()] if m else wikitext


def _drop_refs(t):
    """mwparserfromhell.strip_code() keeps the TEXT INSIDE <ref>...</ref>, so a
    citation leaks into the prose ("...relativity.cite"). Strip refs, comments
    and tables before parsing, whichever backend follows."""
    t = re.sub(r"<ref[^>]*/>", "", t)
    t = re.sub(r"<ref.*?</ref>", "", t, flags=re.S | re.I)
    t = re.sub(r"<!--.*?-->", "", t, flags=re.S)
    t = re.sub(r"\{\|.*?\|\}", "", t, flags=re.S)  # tables
    return t


def strip_markup(wikitext):
    """Prefer mwparserfromhell; fall back to regex if it is unavailable."""
    wikitext = _drop_refs(wikitext)
    try:
        import mwparserfromhell

        code = mwparserfromhell.parse(wikitext)
        # strip_code drops templates, refs, tables, file links and formatting,
        # keeping the visible text of piped links.
        return code.strip_code(normalize=True, collapse=True)
    except ImportError:
        t = wikitext
        t = re.sub(r"<ref[^>]*/>", "", t)
        t = re.sub(r"<ref.*?</ref>", "", t, flags=re.S)
        t = re.sub(r"<!--.*?-->", "", t, flags=re.S)
        t = re.sub(r"\{\|.*?\|\}", "", t, flags=re.S)  # tables
        for _ in range(6):  # nested templates
            t = re.sub(r"\{\{[^{}]*\}\}", "", t, flags=re.S)
        t = re.sub(r"\[\[(?:File|Image|Category):[^\]]*\]\]", "", t, flags=re.I)
        t = re.sub(r"\[\[[^\]|]*\|([^\]]*)\]\]", r"\1", t)
        t = re.sub(r"\[\[([^\]]*)\]\]", r"\1", t)
        t = re.sub(r"</?[^>]+>", "", t)
        t = t.replace("'''", "").replace("''", "")
        return t


def clean(text, max_chars):
    text = to_ascii(strip_markup(text))
    text = re.sub(r"\s+", " ", text).strip()
    text = text.replace("\t", " ")
    if len(text) > max_chars:  # trim at a sentence, else a word
        cut = text[:max_chars]
        dot = cut.rfind(". ")
        text = (cut[: dot + 1] if dot > max_chars * 0.5 else cut.rsplit(" ", 1)[0]).rstrip(",;: ")
    return text


_KEY_BAD = re.compile(r"[^a-z0-9'\-]")


def make_key(title):
    k = to_ascii(title).lower().strip().replace(" ", "-")
    k = _KEY_BAD.sub("", k)
    return k[:31]  # the index stores 31 bytes + NUL


# --------------------------------------------------------------------------

def build(dump_path, max_chars, min_chars):
    # Dumps ship with export-0.10 and export-0.11 namespaces; read it off the
    # first <page> rather than hardcoding a version that will age out.
    ns = None
    opener = bz2.open if dump_path.endswith(".bz2") else open

    lines, seen = [], set()
    kept = skipped_redirect = skipped_ns = skipped_short = 0

    with opener(dump_path, "rb") as fh:
        for _, elem in ET.iterparse(fh, events=("end",)):
            if not elem.tag.endswith("page"):
                continue
            if ns is None:
                ns = elem.tag[: elem.tag.rindex("}") + 1] if "}" in elem.tag else ""

            ns_el = elem.find(f"{ns}ns")
            if ns_el is None or ns_el.text != "0":  # articles only
                skipped_ns += 1
                elem.clear()
                continue
            if elem.find(f"{ns}redirect") is not None:
                skipped_redirect += 1
                elem.clear()
                continue

            title_el = elem.find(f"{ns}title")
            text_el = elem.find(f"{ns}revision/{ns}text")
            if title_el is None or text_el is None or not text_el.text:
                elem.clear()
                continue

            title = title_el.text
            key = make_key(title)
            if not key or key in seen:
                elem.clear()
                continue

            body = clean(lead_section(text_el.text), max_chars)
            if len(body) < min_chars:
                skipped_short += 1
                elem.clear()
                continue

            entry = f"{to_ascii(title)}: {body}"
            lines.append(f"{key}\t{entry}\n")
            seen.add(key)
            kept += 1
            if kept % 20000 == 0:
                print(f"  {kept:,} articles...", file=sys.stderr)
            elem.clear()

    print(f"kept {kept:,}  (skipped: {skipped_redirect:,} redirects, "
          f"{skipped_ns:,} non-article pages, {skipped_short:,} stubs)")
    lines.sort()  # WCDB requires globally sorted keys
    return lines


def verify(path, key=None):
    """Read the file back the way the device does, so a bad build is caught here
    rather than on the card."""
    d = open(path, "rb").read()
    assert d[:4] == b"WCDB", "bad magic"
    bc, total = struct.unpack_from("<II", d, 4)
    print(f"{path}: {total:,} entries, {bc:,} blocks")

    prev = ""
    for i in range(bc):
        p = 12 + 44 * i
        fw = d[p:p + 32].split(b"\0")[0].decode()
        off, csz, rsz = struct.unpack_from("<III", d, p + 32)
        assert rsz <= BLOCK_SIZE, f"block {i} rawSize {rsz} > BLOCK_SIZE"
        raw = zlib.decompressobj(-15).decompress(d[off:off + csz])
        assert len(raw) == rsz, f"block {i} inflates to {len(raw)}, header says {rsz}"
        assert fw >= prev, f"block {i} firstWord out of order: {fw!r} < {prev!r}"
        prev = fw
    print("all blocks inflate, sizes match, index is sorted")

    if key:
        lo, hi, found = 0, bc - 1, 0
        while lo <= hi:  # mirror of the device's on-disk binary search
            mid = (lo + hi) // 2
            fw = d[12 + 44 * mid:12 + 44 * mid + 32].split(b"\0")[0].decode()
            if fw <= key:
                found, lo = mid, mid + 1
            else:
                hi = mid - 1
        p = 12 + 44 * found
        off, csz, _ = struct.unpack_from("<III", d, p + 32)
        raw = zlib.decompressobj(-15).decompress(d[off:off + csz]).decode()
        for line in raw.split("\n"):
            if line.startswith(key + "\t"):
                print(f"\n{key} -> {line.split(chr(9), 1)[1][:300]}...")
                return
        print(f"\n{key}: not found (searched block {found})")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("dump", help="simplewiki-latest-pages-articles.xml(.bz2), or the .cdb with --verify")
    ap.add_argument("--output", default="wikipedia.cdb")
    ap.add_argument("--max-chars", type=int, default=3000, help="cap per article")
    ap.add_argument("--min-chars", type=int, default=80, help="drop stubs shorter than this")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--key", default=None, help="with --verify: look one key up")
    a = ap.parse_args()

    if a.verify:
        verify(a.dump, a.key)
    else:
        write_cdb(build(a.dump, a.max_chars, a.min_chars), a.output)
