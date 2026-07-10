#!/usr/bin/env python3
"""
prepare_dict_fat.py — Build the FAT (uncapped meanings) dictionary.cdb.
Same pipeline as prepare_dict_compressed.py with two changes:
  1. All meanings kept (no [:3] cap).
  2. Block splitter aligns BACKWARD to the previous newline, so no raw
     block ever exceeds BLOCK_SIZE (fixes a latent overflow where blocks
     could exceed the sketch's decompression buffer).
Usage: python3 prepare_dict_fat.py [--output dictionary.cdb]
"""
import json, string, struct, zlib, os, sys, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prepare_dict_compressed as P

def build_lines():
    raw = {}
    print('Loading Wordset...')
    raw_entries = P.download_wordset()
    lines, seen = [], set()
    for word, data in raw_entries.items():
        if not P.is_good_word(word): continue
        meanings = data.get('meanings', [])
        if not meanings: continue
        w = word.lower()
        if w in seen: continue
        seen.add(w)
        parts = []
        for m in meanings:                       # uncapped
            sp = m.get('speech_part',''); d = m.get('def','').strip()
            if not d: continue
            d = P.compress_text(d)
            ab = P.SPEECH_ABBREV.get(sp, sp)
            parts.append(f'({ab}) {d}' if sp else d)
        defn = '; '.join(parts)
        if len(defn) < 3: continue
        lines.append(f'{w}\t{defn}\n')
    lines.sort()
    return lines

def split_blocks(raw_text, block_size):
    """Line-aligned blocks that never exceed block_size."""
    blocks, i = [], 0
    n = len(raw_text)
    while i < n:
        end = min(i + block_size, n)
        if end < n:
            nl = raw_text.rfind(b'\n', i, end)
            if nl <= i:                          # single line > block_size
                nl = raw_text.find(b'\n', end)
                end = n if nl < 0 else nl + 1
            else:
                end = nl + 1
        blocks.append(raw_text[i:end])
        i = end
    return blocks

def write_cdb(lines, output_path):
    raw_text = ''.join(lines).encode('utf-8')
    blocks_raw = split_blocks(raw_text, P.BLOCK_SIZE)
    assert all(len(b) <= P.BLOCK_SIZE for b in blocks_raw)
    blocks_c = []
    for b in blocks_raw:
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        blocks_c.append(c.compress(b) + c.flush())
    first_words = [b.split(b'\n')[0].split(b'\t')[0].decode() for b in blocks_raw]
    bc = len(blocks_c)
    data_start = 12 + bc * 44
    offs, cur = [], data_start
    for cb in blocks_c:
        offs.append(cur); cur += len(cb)
    with open(output_path, 'wb') as f:
        f.write(b'WCDB')
        f.write(struct.pack('<I', bc))
        f.write(struct.pack('<I', len(lines)))
        for i in range(bc):
            wb = first_words[i].encode()[:31]
            f.write(wb + b'\0' * (32 - len(wb)))
            f.write(struct.pack('<III', offs[i], len(blocks_c[i]), len(blocks_raw[i])))
        for cb in blocks_c:
            f.write(cb)
    print(f'{len(lines)} entries, {bc} blocks, '
          f'{os.path.getsize(output_path):,} bytes')

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', default='dictionary.cdb')
    a = ap.parse_args()
    write_cdb(build_lines(), a.output)
