#!/usr/bin/env python3
"""
prepare_dict_compressed.py — Build a block-compressed dictionary for Watchy.

Creates a .cdb (Compressed Dictionary Binary) file with:
  - Block index (first word of each block for binary search)
  - Individually compressed 16KB blocks (raw deflate)
  - Full untruncated definitions with abbreviation compression

The ESP32 decompresses one 16KB block per lookup using ROM miniz.

Usage:
    python3 prepare_dict_compressed.py [--output FILE]
"""

import json, os, string, struct, zlib, sys

try:
    import urllib.request
except ImportError:
    pass

WORDSET_URL = "https://raw.githubusercontent.com/wordset/wordset-dictionary/master/data/{}.json"
FREQ_URL = "https://raw.githubusercontent.com/first20hours/google-10000-english/master/20k.txt"

SPEECH_ABBREV = {
    "noun": "n.", "verb": "v.", "adjective": "adj.", "adverb": "adv.",
    "preposition": "prep.", "conjunction": "conj.", "interjection": "interj.",
    "pronoun": "pron.", "determiner": "det.", "auxiliary verb": "aux.",
}

COMPRESSIONS = [
    ("of or relating to", "rel. to"), ("of or pertaining to", "rel. to"),
    ("the act or process of", "act of"), ("the act of", "act of"),
    ("the state of being", "being"), ("the quality of being", "being"),
    ("a person who", "one who"), ("someone who is", "one who is"),
    ("someone who", "one who"), ("characterized by", "marked by"),
    ("especially", "esp."), ("something", "sth"),
    ("relating to", "rel. to"), ("pertaining to", "rel. to"),
    ("particularly", "esp."), ("frequently", "often"),
    ("approximately", "approx."), ("for example", "e.g."),
]

BLOCK_SIZE = 32768  # 32KB uncompressed blocks
WORD_FIELD_LEN = 32  # fixed-width word field in index


def download_wordset(cache_dir="wordset_cache"):
    os.makedirs(cache_dir, exist_ok=True)
    all_entries = {}
    for letter in string.ascii_lowercase:
        path = os.path.join(cache_dir, f"{letter}.json")
        if os.path.exists(path):
            print(f"  {letter} (cached)", end="", flush=True)
        else:
            print(f"  {letter} (downloading)", end="", flush=True)
            try:
                urllib.request.urlretrieve(WORDSET_URL.format(letter), path)
            except Exception as e:
                print(f" FAILED: {e}")
                continue
        with open(path, "r", encoding="utf-8") as f:
            all_entries.update(json.load(f))
    print()
    return all_entries


def compress_text(d):
    for long_form, short_form in COMPRESSIONS:
        d = d.replace(long_form, short_form)
    for prefix in ["a ", "an ", "the "]:
        if d.startswith(prefix) and len(d) > 15:
            d = d[len(prefix):]
    return d


def format_definition(meanings):
    parts = []
    for m in meanings[:3]:
        sp = m.get("speech_part", "")
        d = m.get("def", "").strip()
        if not d:
            continue
        d = compress_text(d)
        ab = SPEECH_ABBREV.get(sp, sp)
        parts.append(f"({ab}) {d}" if sp else d)
    return "; ".join(parts)


def is_good_word(word):
    if len(word) < 2 or len(word) > 28 or " " in word:
        return False
    return all(c.isalpha() or c in "-'" for c in word)


def build_dictionary_text(raw_dict):
    """Build sorted, deduplicated dictionary lines."""
    lines = []
    seen = set()
    for word, data in raw_dict.items():
        if not is_good_word(word):
            continue
        meanings = data.get("meanings", [])
        if not meanings:
            continue
        # Build synonym redirect if possible
        w = word.lower()
        if w in seen:
            continue
        seen.add(w)

        defn = format_definition(meanings)
        if len(defn) < 3:
            continue
        lines.append(f"{w}\t{defn}\n")

    lines.sort()
    return lines


def build_compressed_file(lines, output_path):
    """Create the .cdb compressed dictionary file."""
    # Join all lines into one big text
    raw_text = "".join(lines).encode("utf-8")
    print(f"Raw dictionary: {len(lines)} entries, {len(raw_text) / 1024:.0f} KB")

    # Split into blocks aligned to line boundaries
    blocks_raw = []
    i = 0
    while i < len(raw_text):
        end = min(i + BLOCK_SIZE, len(raw_text))
        # Align to newline
        while end < len(raw_text) and raw_text[end : end + 1] != b"\n":
            end += 1
        if end < len(raw_text):
            end += 1
        blocks_raw.append(raw_text[i:end])
        i = end

    # Compress each block (raw deflate, no headers)
    blocks_compressed = []
    for block in blocks_raw:
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15)  # raw deflate
        compressed = compressor.compress(block) + compressor.flush()
        blocks_compressed.append(compressed)

    # Extract first word of each block for the index
    first_words = []
    for block in blocks_raw:
        line = block.split(b"\n")[0].decode("utf-8")
        word = line.split("\t")[0]
        first_words.append(word)

    # Calculate offsets
    block_count = len(blocks_compressed)
    # Header: magic(4) + block_count(4) + entry_count(4) = 12
    # Index: block_count * (32 + 4 + 4 + 4) = block_count * 44
    header_size = 12
    index_entry_size = WORD_FIELD_LEN + 4 + 4 + 4  # word + offset + comp_size + uncomp_size
    index_size = block_count * index_entry_size
    data_start = header_size + index_size

    # Calculate data offsets
    offsets = []
    current_offset = data_start
    for comp_block in blocks_compressed:
        offsets.append(current_offset)
        current_offset += len(comp_block)

    # Write the file
    with open(output_path, "wb") as f:
        # Header
        f.write(b"WCDB")
        f.write(struct.pack("<I", block_count))
        f.write(struct.pack("<I", len(lines)))

        # Index
        for i in range(block_count):
            # Word field (32 bytes, null-padded)
            word_bytes = first_words[i].encode("utf-8")[:WORD_FIELD_LEN - 1]
            word_bytes += b"\x00" * (WORD_FIELD_LEN - len(word_bytes))
            f.write(word_bytes)
            f.write(struct.pack("<I", offsets[i]))
            f.write(struct.pack("<I", len(blocks_compressed[i])))
            f.write(struct.pack("<I", len(blocks_raw[i])))

        # Compressed data blocks
        for comp_block in blocks_compressed:
            f.write(comp_block)

    file_size = os.path.getsize(output_path)
    total_compressed = sum(len(b) for b in blocks_compressed)
    print(f"Blocks: {block_count} × ~{BLOCK_SIZE // 1024}KB")
    print(f"Index: {index_size / 1024:.1f} KB")
    print(f"Compressed data: {total_compressed / 1024:.0f} KB")
    print(f"Total file: {file_size / 1024:.0f} KB")
    print(f"Compression ratio: {file_size * 100 / len(raw_text):.0f}%")

    # Samples
    import random
    samples = random.sample(lines, min(6, len(lines)))
    print("\nSample entries:")
    for line in sorted(samples):
        print(f"  {line.strip()[:80]}")


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Build compressed Watchy dictionary")
    parser.add_argument("--output", default="dictionary.cdb")
    args = parser.parse_args()

    print("Loading Wordset dictionary...")
    raw_dict = download_wordset()
    print(f"Total raw entries: {len(raw_dict)}")

    print("\nBuilding dictionary...")
    lines = build_dictionary_text(raw_dict)

    print("\nCompressing...")
    build_compressed_file(lines, args.output)


if __name__ == "__main__":
    main()
