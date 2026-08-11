#!/usr/bin/env python3
"""
make_thesaurus.py — Build thesaurus.cdb for the CrossPoint Almanac.

Data sources (no hand-entered or model-generated text anywhere):

  1. Open English WordNet, WNDB distribution — sense-grouped synonyms and
     antonyms, with part of speech.
     https://en-word.net/  (releases: github.com/globalwordnet/english-wordnet)
     Derived from Princeton WordNet under the WordNet License and further
     developed under CC BY 4.0. Attribution is required to BOTH Princeton
     WordNet and the Open English WordNet team. Take the plain edition, not
     the "+" edition: proper nouns have no place in a thesaurus.

  2. Moby Thesaurus II (mthesaur.txt) — breadth. Public domain by grant from
     Grady Ward, January 2001. Project Gutenberg etext #3202.
     https://www.gutenberg.org/ebooks/3202
     30,260 root words, 2,520,264 related terms. Plain ASCII, CRLF, accents
     already stripped, one comma-separated line per root word.

Neither source is downloaded by this script and neither .cdb should be
committed — build locally, as with make_wikipedia.py. See
docs/almanac-data-provenance.md.

WHY THE TWO SOURCES ARE KEPT IN SEPARATE BUCKETS
------------------------------------------------
WordNet synsets carry part of speech and sense grouping, so its synonyms are
emitted as "(adj) ...; (n) ..." blocks straight out of the data. Moby's lists
are flat: no part of speech, no sense grouping. Splitting Moby's terms across
those blocks would mean GUESSING which sense each term belongs to -- "light"
is noun, verb and adjective -- and that guess cannot be checked against
anything. So Moby lands in one trailing "(rel)" bucket instead. Every string
in the output then traces to exactly one source file, and --verify can prove
it by set membership.

Output format: WCDB — identical to dictionary.cdb, gazetteer.cdb and
wikipedia.cdb, so the on-device engine needs no changes. Keys follow
make_wikipedia.py's rule (lowercase ASCII, spaces -> '-'), which is what keeps
Python's lines.sort() byte-identical to the device's lower() comparison in
WcdbReader::findBlock().

Entry body mirrors the dictionary's own "(ab) text; (ab) text" grammar:

  (adj) felicitous, glad; fortunate, lucky; (n) ...; (opp) unhappy;
  (rel) blissful, blithe, cheerful, chipper, ...

Moby's related-term lists are ALPHABETICAL. Cutting one at N terms or N
characters therefore keeps a-through-d and silently drops the rest of the
alphabet. So when a cap has to bite, this script SAMPLES the list evenly rather
than truncating it: the entry gets fewer terms, but they still span a to z.

Usage:
  python3 make_thesaurus.py --wordnet path/to/oewn/dict --moby mthesaur.txt
  python3 make_thesaurus.py --wordnet dict --moby mthesaur.txt --max-related 40
  python3 make_thesaurus.py --verify thesaurus.cdb [--key happy]
  python3 make_thesaurus.py --verify thesaurus.cdb --wordnet dict --moby mthesaur.txt
"""
import argparse
import os
import re
import struct
import sys
import unicodedata
import zlib

BLOCK_SIZE = 32768  # matches prepare_dict_compressed.py; blocks stay <= this

# A single record must fit inside one block or split_blocks() cannot place it
# and write_cdb()'s assert fires. This is the hard ceiling; --max-chars is the
# readability cap and should sit far below it.
# Two ceilings apply, and the smaller one is RAM, not the file format:
#   - a record must fit inside one block or split_blocks() cannot place it,
#     so the format ceiling is BLOCK_SIZE minus key, tab and newline (~32,700)
#   - but render() builds wrapped_ as a vector<std::string>, one per display
#     line, on top of the 32KB decBuf_ and 34KB compBuf_ the reader already
#     reserves. A 30,000-char entry wraps to ~750 lines and costs well over
#     50KB of heap to display. 12,000 chars is roughly 300 lines, which the
#     device renders comfortably.
MAX_ENTRY_BYTES = 12000

# Same shape as prepare_dict_compressed.py::SPEECH_ABBREV. If that module uses
# different strings, change these to match so the two modules read alike.
POS_ABBREV = {"n": "n", "v": "v", "a": "adj", "r": "adv"}

# WNDB index.<pos> -> data.<pos>
POS_FILES = {"n": "noun", "v": "verb", "a": "adj", "r": "adv"}


# --------------------------------------------------------------------------
# WCDB writer — byte-identical to prepare_dict_fat.py::write_cdb.
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

    print(f"{len(lines)} headwords, {bc} blocks, {os.path.getsize(output_path):,} bytes")
    print(f"index on card: {bc * 44:,} bytes (never loaded into RAM)")


# --------------------------------------------------------------------------
# Keys — identical rule to make_wikipedia.py::make_key
# --------------------------------------------------------------------------

_KEY_BAD = re.compile(r"[^a-z0-9'\-]")


def to_ascii(s):
    """The device font has ASCII glyphs only."""
    s = unicodedata.normalize("NFKD", str(s))
    return s.encode("ascii", "ignore").decode("ascii")


def make_key(word):
    k = to_ascii(word).lower().strip().replace(" ", "-").replace("_", "-")
    k = _KEY_BAD.sub("", k)
    return k[:31]  # the index stores 31 bytes + NUL


def display(word):
    """Readable form: WordNet uses '_' for spaces, Moby uses spaces already."""
    return to_ascii(word).replace("_", " ").strip()


# --------------------------------------------------------------------------
# Open English WordNet (WNDB format)
# --------------------------------------------------------------------------

_ADJ_MARKER = re.compile(r"\([aip]{1,2}\)$")


def _clean_word(w):
    """Strip the adjective syntactic markers (p) (a) (ip) that WNDB appends."""
    return _ADJ_MARKER.sub("", w)


def parse_data_file(path):
    """data.<pos> -> {offset: (ss_type, [words], [(sym, off, pos, src_tgt)])}"""
    synsets = {}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith(" ") or not line.strip():
                continue  # the license block at the top of every WNDB file
            meta = line.split(" | ", 1)[0]
            f = meta.split()
            try:
                offset, ss_type = f[0], f[2]
                w_cnt = int(f[3], 16)
                i = 4
                words = []
                for _ in range(w_cnt):
                    words.append(_clean_word(f[i]))
                    i += 2  # word, lex_id
                p_cnt = int(f[i])
                i += 1
                ptrs = []
                for _ in range(p_cnt):
                    ptrs.append((f[i], f[i + 1], f[i + 2], f[i + 3]))
                    i += 4
            except (IndexError, ValueError):
                continue  # malformed line; never abort a build on one record
            synsets[offset] = (ss_type, words, ptrs)
    return synsets


def parse_index_file(path):
    """index.<pos> -> {lemma: [offset, ...]} in WordNet's sense-rank order.

    Sense order matters: it decides what survives --max-chars, so the common
    sense of a word is the one that stays.
    """
    index = {}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith(" ") or not line.strip():
                continue
            f = line.split()
            try:
                lemma = f[0]
                synset_cnt = int(f[2])
                p_cnt = int(f[3])
                i = 4 + p_cnt  # skip the pointer symbols
                offsets = f[i + 2 : i + 2 + synset_cnt]  # skip sense_cnt, tagsense_cnt
            except (IndexError, ValueError):
                continue
            if offsets:
                index[lemma] = offsets
    return index


def load_wordnet(dict_dir):
    """-> {lemma: {pos: [[synonym, ...] per sense]}}, {lemma: {pos: [antonym]}}"""
    syn, ant, found = {}, {}, []
    for pos, stem in POS_FILES.items():
        data_path = os.path.join(dict_dir, f"data.{stem}")
        index_path = os.path.join(dict_dir, f"index.{stem}")
        if not (os.path.exists(data_path) and os.path.exists(index_path)):
            print(f"  no {stem} files in {dict_dir}, skipping", file=sys.stderr)
            continue
        found.append(stem)
        synsets = parse_data_file(data_path)
        index = parse_index_file(index_path)
        print(f"  {stem}: {len(synsets):,} synsets, {len(index):,} lemmas", file=sys.stderr)

        for lemma, offsets in index.items():
            groups, antonyms = [], []
            for off in offsets:
                s = synsets.get(off)
                if not s:
                    continue
                _, words, ptrs = s
                mates = [w for w in words if w.lower() != lemma.lower()]
                if mates:
                    groups.append(mates)
                for sym, toff, tpos, _st in ptrs:
                    if sym != "!":
                        continue
                    t = synsets.get(toff) if tpos == pos else None
                    # Cross-POS antonym pointers need the other file; the same-POS
                    # case covers effectively all of them and needs no lookup table.
                    if t:
                        antonyms.extend(w for w in t[1] if w.lower() != lemma.lower())
            if groups:
                syn.setdefault(lemma, {})[pos] = groups
            if antonyms:
                ant.setdefault(lemma, {})[pos] = antonyms

    # A thesaurus with no WordNet in it is not the thing that was asked for, and
    # silently shipping a Moby-only build would look like success. Stop here.
    if not found:
        sys.exit(f"error: no WNDB files under {dict_dir!r}\n"
                 f"       expected index.noun / data.noun / index.adj / ... in that directory\n"
                 f"       get english-wordnet-2025.zip (WNDB) from https://en-word.net/downloads")
    return syn, ant


# --------------------------------------------------------------------------
# Moby Thesaurus II
# --------------------------------------------------------------------------


def load_moby(path):
    """-> {root: [related, ...]} in the file's own order.

    The order is left exactly as Moby has it. Re-ranking would be an editorial
    judgement with no source behind it.
    """
    moby = {}
    # Moby is plain ASCII with CRLF; latin-1 never raises on a stray byte.
    with open(path, "r", encoding="latin-1") as fh:
        for line in fh:
            line = line.strip()
            if not line or "," not in line:
                continue
            parts = [p.strip() for p in line.split(",")]
            root, terms = parts[0], [p for p in parts[1:] if p]
            if root and terms:
                moby[root] = terms
    return moby


# --------------------------------------------------------------------------
# Merge
# --------------------------------------------------------------------------


def sample_evenly(seq, k):
    """Take k items spread across seq, keeping the original order.

    Moby's related-term lists are ALPHABETICAL, so taking the first k gives you
    a-through-b and drops the rest of the alphabet. Taking every (n/k)th item
    instead spans the whole list. This is not a ranking -- there is no
    importance signal in Moby to rank on -- it is just an even cut, so no
    editorial judgement enters the data.
    """
    n = len(seq)
    if k <= 0 or n <= k:
        return list(seq)
    return [seq[(i * n) // k] for i in range(k)]


def dedupe(seq):
    """Order-preserving, case-insensitive."""
    out, seen = [], set()
    for x in seq:
        k = x.lower()
        if k and k not in seen:
            seen.add(k)
            out.append(x)
    return out


def build_body(headword, wn_groups, wn_antonyms, moby_terms, max_chars, max_related):
    """Assemble one entry body in the dictionary's "(ab) text; (ab) text" style."""
    chunks = []
    shown = {headword.lower()}

    for pos in ("a", "n", "v", "r"):  # adjectives first: the thesaurus case
        groups = wn_groups.get(pos)
        if not groups:
            continue
        rendered = []
        for mates in groups:
            mates = dedupe(display(m) for m in mates)
            if mates:
                rendered.append(", ".join(mates))
                shown.update(m.lower() for m in mates)
        if rendered:
            chunks.append(f"({POS_ABBREV[pos]}) " + "; ".join(rendered))

    antonyms = dedupe(display(a) for pos in wn_antonyms for a in wn_antonyms[pos])
    antonyms = [a for a in antonyms if a.lower() != headword.lower()]
    if antonyms:
        chunks.append("(opp) " + ", ".join(antonyms))
        shown.update(a.lower() for a in antonyms)

    # Moby last, with anything already shown above removed so the bucket adds
    # breadth rather than repeating WordNet back at the reader.
    related = [t for t in dedupe(display(t) for t in moby_terms) if t.lower() not in shown]
    if max_related > 0:
        related = sample_evenly(related, max_related)

    def assemble(rel):
        parts = chunks + (["(rel) " + ", ".join(rel)] if rel else [])
        out = "; ".join(parts).replace("\t", " ").replace("\n", " ")
        return re.sub(r"\s+", " ", out).strip()

    limit = min(max_chars, MAX_ENTRY_BYTES)
    body = assemble(related)
    if len(body) > limit and related:
        # Thin the Moby bucket evenly until it fits, instead of chopping its
        # tail -- a tail chop would silently drop everything after whatever
        # letter the budget happened to run out on. WordNet's own synonyms are
        # never thinned: they are sense-grouped and small.
        lo, hi, best = 1, len(related), None
        while lo <= hi:
            mid = (lo + hi) // 2
            candidate = assemble(sample_evenly(related, mid))
            if len(candidate) <= limit:
                best, lo = candidate, mid + 1
            else:
                hi = mid - 1
        body = best if best is not None else assemble([])

    if len(body) > limit:  # WordNet alone overruns: rare, but clamp regardless
        cut = body[:limit]
        comma = cut.rfind(", ")
        body = (cut[:comma] if comma > limit * 0.5 else cut.rsplit(" ", 1)[0]).rstrip(",;: ")
    return body


def check_sources(dict_dir, moby_path):
    """Fail before the slow part, not thirty seconds into it."""
    if not os.path.isdir(dict_dir):
        sys.exit(f"error: --wordnet {dict_dir!r} is not a directory\n"
                 f"       unzip english-wordnet-2025.zip (WNDB) from https://en-word.net/downloads\n"
                 f"       and point --wordnet at the folder holding index.noun / data.noun")
    if not os.path.isfile(moby_path):
        sys.exit(f"error: --moby {moby_path!r} not found\n"
                 f"       mthesaur.txt (23.7 MB) is inside files.zip at "
                 f"https://www.gutenberg.org/files/3202/\n"
                 f"       note the ebook page's 'Plain Text' link is the README, not the data")


def build(dict_dir, moby_path, max_chars, max_related):
    check_sources(dict_dir, moby_path)
    print("Loading Open English WordNet...", file=sys.stderr)
    wn_syn, wn_ant = load_wordnet(dict_dir)
    print(f"Loading Moby Thesaurus from {moby_path}...", file=sys.stderr)
    moby = load_moby(moby_path)
    print(f"  {len(moby):,} root words", file=sys.stderr)
    if len(moby) < 1000:
        sys.exit(f"error: {moby_path!r} yielded only {len(moby):,} root words; the real "
                 f"mthesaur.txt has 30,260.\n"
                 f"       this looks like the README rather than the thesaurus data")

    # Headword set: the union, keyed so "a great deal" and "a_great_deal" meet.
    headwords = {}
    for lemma in wn_syn:
        headwords.setdefault(make_key(lemma), display(lemma))
    for root in moby:
        headwords.setdefault(make_key(root), display(root))

    lines, seen = [], set()
    kept = skipped_empty = skipped_nokey = 0

    for key, headword in headwords.items():
        if not key or key in seen:
            skipped_nokey += 1
            continue

        # Look the sources up under their own spellings.
        wn_key = headword.replace(" ", "_")
        groups = wn_syn.get(wn_key) or wn_syn.get(headword) or {}
        antonyms = wn_ant.get(wn_key) or wn_ant.get(headword) or {}
        terms = moby.get(headword) or moby.get(headword.replace("_", " ")) or []

        body = build_body(headword, groups, antonyms, terms, max_chars, max_related)
        if not body:
            skipped_empty += 1  # a lemma whose only synset member is itself
            continue

        # The key is displayed as the headword on device; add the readable form
        # only when they differ, as make_wikipedia.py does for titles.
        entry = body if key == headword.lower() else f"{headword}: {body}"
        lines.append(f"{key}\t{entry}\n")
        seen.add(key)
        kept += 1
        if kept % 20000 == 0:
            print(f"  {kept:,} headwords...", file=sys.stderr)

    print(f"kept {kept:,}  (skipped: {skipped_empty:,} with no synonym but themselves, "
          f"{skipped_nokey:,} unkeyable or duplicate)")
    lines.sort()  # WCDB requires globally sorted keys
    return lines


# --------------------------------------------------------------------------
# Verify
# --------------------------------------------------------------------------


def verify(path, key=None, dict_dir=None, moby_path=None):
    """Read the file back the way the device does, so a bad build is caught here
    rather than on the card."""
    d = open(path, "rb").read()
    assert d[:4] == b"WCDB", "bad magic"
    bc, total = struct.unpack_from("<II", d, 4)
    print(f"{path}: {total:,} entries, {bc:,} blocks")

    prev = ""
    prev_key = ""
    for i in range(bc):
        p = 12 + 44 * i
        fw = d[p:p + 32].split(b"\0")[0].decode()
        off, csz, rsz = struct.unpack_from("<III", d, p + 32)
        assert rsz <= BLOCK_SIZE, f"block {i} rawSize {rsz} > BLOCK_SIZE"
        raw = zlib.decompressobj(-15).decompress(d[off:off + csz])
        assert len(raw) == rsz, f"block {i} inflates to {len(raw)}, header says {rsz}"
        assert fw >= prev, f"block {i} firstWord out of order: {fw!r} < {prev!r}"
        prev = fw

        # WcdbReader::lookup() early-exits on the first key greater than the
        # query, so a single out-of-order record silently hides every key after
        # it in the block. Check every one, not just the block boundaries.
        for line in raw.decode("utf-8").split("\n"):
            if not line:
                continue
            k = line.split("\t", 1)[0]
            assert k >= prev_key, f"record out of order: {k!r} after {prev_key!r}"
            assert k == k.lower(), f"key not lowercase: {k!r}"
            assert not _KEY_BAD.search(k), f"key outside the device charset: {k!r}"
            prev_key = k
    print("all blocks inflate, sizes match, every record is in device sort order")

    if dict_dir and moby_path:
        audit_provenance(d, bc, dict_dir, moby_path)

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


_TERM_SPLIT = re.compile(r"\((?:n|v|adj|adv|opp|rel)\)")


def audit_provenance(d, bc, dict_dir, moby_path, sample=2000):
    """Every term in the output must appear in one of the two source files.

    This is the check that makes the "no hand-entered or model-generated text"
    claim testable rather than asserted.
    """
    print(f"\nAuditing provenance against the sources (sample of {sample:,})...")
    wn_syn, wn_ant = load_wordnet(dict_dir)
    moby = load_moby(moby_path)

    vocab = set()
    for lemma, per_pos in wn_syn.items():
        vocab.add(display(lemma).lower())
        for groups in per_pos.values():
            for mates in groups:
                vocab.update(display(m).lower() for m in mates)
    for per_pos in wn_ant.values():
        for terms in per_pos.values():
            vocab.update(display(t).lower() for t in terms)
    for root, terms in moby.items():
        vocab.add(display(root).lower())
        vocab.update(display(t).lower() for t in terms)

    checked = orphans = 0
    for i in range(bc):
        p = 12 + 44 * i
        off, csz, _ = struct.unpack_from("<III", d, p + 32)
        raw = zlib.decompressobj(-15).decompress(d[off:off + csz]).decode("utf-8")
        for line in raw.split("\n"):
            if not line or "\t" not in line:
                continue
            body = line.split("\t", 1)[1]
            # build() prefixes "Headword: " only when the key was mangled or
            # truncated. An unprefixed body always opens with a "(tag)", so that
            # is the test -- not a character window, which misses headwords
            # longer than the window and reports them as orphan terms.
            if not body.startswith("(") and ": " in body:
                body = body.split(": ", 1)[1]
            for chunk in _TERM_SPLIT.split(body):
                for term in chunk.replace(";", ",").split(","):
                    term = term.strip()
                    if not term:
                        continue
                    checked += 1
                    if term.lower() not in vocab:
                        orphans += 1
                        if orphans <= 10:
                            print(f"  ORPHAN (in no source): {term!r} in {line[:40]!r}")
            if checked >= sample:
                break
        if checked >= sample:
            break

    if orphans:
        print(f"FAIL: {orphans:,} of {checked:,} terms trace to no source file")
    else:
        print(f"all {checked:,} sampled terms trace to WordNet or Moby")


# --------------------------------------------------------------------------

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--wordnet", help="OEWN WNDB dict/ directory (index.noun, data.noun, ...)")
    ap.add_argument("--moby", help="mthesaur.txt from Project Gutenberg etext 3202")
    ap.add_argument("--output", default="thesaurus.cdb")
    # Caps now thin the Moby bucket by even sampling rather than chopping its
    # tail, so a smaller number costs you density across the whole alphabet
    # instead of everything after the letter the budget ran out on.
    ap.add_argument("--max-chars", type=int, default=6000,
                    help="cap per entry; the (rel) bucket is sampled down to fit")
    ap.add_argument("--max-related", type=int, default=0,
                    help="cap on the Moby (rel) term count; 0 = only --max-chars applies")
    ap.add_argument("--verify", metavar="CDB", help="check a built .cdb instead of building")
    ap.add_argument("--key", default=None, help="with --verify: look one key up")
    a = ap.parse_args()

    if a.verify:
        verify(a.verify, a.key, a.wordnet, a.moby)
    elif a.wordnet and a.moby:
        write_cdb(build(a.wordnet, a.moby, a.max_chars, a.max_related), a.output)
    else:
        ap.error("need --wordnet and --moby to build, or --verify to check")
