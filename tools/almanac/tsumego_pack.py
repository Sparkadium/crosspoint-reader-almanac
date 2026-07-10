#!/usr/bin/env python3
"""
tsumego_pack.py - Convert SGF tsumego collections into a compact binary
format (problems.bin) for WatchyTsumego.

Usage:
  python3 tsumego_pack.py pack out.bin file1.sgf dir2/ ...
  python3 tsumego_pack.py dump out.bin [index]

Binary format (little-endian):
  magic   4 bytes  "TSU1"
  count   u16
  offsets u32 * count            (absolute file offsets)
  blobs   ...

Problem blob:
  flags   u8   bit0: to_play (0=black, 1=white)
               bit1: wall left   bit2: wall top
               bit3: wall right  bit4: wall bottom
  w, h    u8, u8                 (region size in points)
  nBlack  u8, then nBlack pos bytes   (pos = row*w + col)
  nWhite  u8, then nWhite pos bytes
  tree    node = [pos u8][status u8], children follow, 0xFF pops.
          Top-level children sequence ends with 0xFE.
          status: 0=continue, 1=RIGHT (correct), 2=WRONG (explicit)
          pos 0xFD = pass

Problems are normalized (mirrored) so they hug the top-left corner.
Region = bounding box of all stones and tree moves, padded by 2 points
on any side that is open board (not a real board edge).
"""

import sys, os, re, struct, glob

PASS = 0xFD
END_TREE = 0xFE
POP = 0xFF

# ---------------------------------------------------------------- SGF parse

class Node:
    __slots__ = ("props", "children")
    def __init__(self):
        self.props = {}
        self.children = []

def parse_sgf(text):
    """Minimal SGF parser -> root Node. Handles escapes and variations."""
    i, n = 0, len(text)

    def skip_ws():
        nonlocal i
        while i < n and text[i] in " \t\r\n":
            i += 1

    def parse_gametree():
        nonlocal i
        assert text[i] == "(", f"expected ( at {i}"
        i += 1
        first = None
        cur = None
        while True:
            skip_ws()
            if i >= n:
                break
            c = text[i]
            if c == ";":
                i += 1
                node = parse_node()
                if cur is None:
                    first = node
                else:
                    cur.children.append(node)
                cur = node
            elif c == "(":
                sub = parse_gametree()
                if cur is None:
                    raise ValueError("variation before first node")
                cur.children.append(sub)
            elif c == ")":
                i += 1
                break
            else:
                raise ValueError(f"unexpected {c!r} at {i}")
        return first

    def parse_node():
        nonlocal i
        node = Node()
        while True:
            skip_ws()
            m = re.match(r"[A-Za-z]+", text[i:])
            if not m:
                break
            ident = m.group(0).upper()
            i += len(m.group(0))
            vals = []
            skip_ws()
            while i < n and text[i] == "[":
                i += 1
                buf = []
                while i < n:
                    c = text[i]
                    if c == "\\" and i + 1 < n:
                        buf.append(text[i + 1]); i += 2; continue
                    if c == "]":
                        i += 1; break
                    buf.append(c); i += 1
                vals.append("".join(buf))
                skip_ws()
            node.props.setdefault(ident, []).extend(vals)
        return node

    skip_ws()
    return parse_gametree()

def coord(s):
    """SGF two-letter coord -> (col, row); '' or 'tt' (on <=19) = pass."""
    if s == "" or s == "tt":
        return None
    return (ord(s[0]) - 97, ord(s[1]) - 97)

# ---------------------------------------------------------------- move tree

class MTree:
    __slots__ = ("move", "color", "status", "comment", "children")
    def __init__(self, move, color, comment=""):
        self.move = move          # (c,r) or None for pass
        self.color = color        # 'B'/'W'
        self.status = 0
        self.comment = comment
        self.children = []

RIGHT_RE = re.compile(r"\b(RIGHT|CORRECT)\b", re.I)
WRONG_RE = re.compile(r"\b(WRONG|FAIL|FAILURE)\b", re.I)

def build_mtree(sgf_node):
    """Convert SGF node tree (after root) into MTree list of first moves."""
    def conv(node):
        out = []
        for ch in node.children:
            out.extend(conv_node(ch))
        return out

    def conv_node(node):
        mv = None
        if "B" in node.props:
            mv = MTree(coord(node.props["B"][0]), "B",
                       node.props.get("C", [""])[0])
        elif "W" in node.props:
            mv = MTree(coord(node.props["W"][0]), "W",
                       node.props.get("C", [""])[0])
        kids = []
        for ch in node.children:
            kids.extend(conv_node(ch))
        if mv is None:
            # setup-only / comment-only node: splice children upward
            return kids
        mv.children = kids
        return [mv]

    return conv(sgf_node)

def annotate(tree_roots, has_right):
    """Set status on terminal nodes. Returns True if any correct line."""
    any_right = False

    def walk(node, mainline_ok):
        nonlocal any_right
        c = node.comment or ""
        if not node.children:  # terminal
            if RIGHT_RE.search(c):
                node.status = 1
            elif WRONG_RE.search(c):
                node.status = 2
            elif not has_right and mainline_ok:
                node.status = 1  # fallback: first-branch mainline is correct
            if node.status == 1:
                any_right = True
        else:
            if RIGHT_RE.search(c):  # RIGHT on a non-terminal: honor it
                node.status = 1
                any_right = True
            for i, ch in enumerate(node.children):
                walk(ch, mainline_ok and i == 0)

    for i, r in enumerate(tree_roots):
        walk(r, i == 0)
    return any_right

# ---------------------------------------------------------------- packing

def collect_moves(tree_roots):
    out = []
    def walk(n):
        if n.move is not None:
            out.append(n.move)
        for ch in n.children:
            walk(ch)
    for r in tree_roots:
        walk(r)
    return out

def transform(pt, sz, flip_x, flip_y):
    c, r = pt
    if flip_x: c = sz - 1 - c
    if flip_y: r = sz - 1 - r
    return (c, r)

def pack_sgf(path, text, warnings):
    root = parse_sgf(text.lstrip("\ufeff \t\r\n"))
    sz = int(root.props.get("SZ", ["19"])[0].split(":")[0])
    black = [coord(v) for v in root.props.get("AB", [])]
    white = [coord(v) for v in root.props.get("AW", [])]
    tree_roots = build_mtree(root)
    if not tree_roots:
        warnings.append(f"{path}: no solution tree, skipped")
        return None
    has_right = bool(RIGHT_RE.search(text))
    annotate(tree_roots, has_right)
    to_play = tree_roots[0].color
    pl = root.props.get("PL", [""])[0].upper()
    if pl in ("B", "W") and pl != to_play:
        warnings.append(f"{path}: PL[{pl}] disagrees with first move; using tree")
    return pack_from_parts(path, sz, black, white, tree_roots, to_play, warnings)

def pack_json(path, text, warnings):
    """sanderland/tsumego JSON: SOL = alternative correct moves, one color."""
    import json as _json
    d = _json.loads(text)
    sz = int(d.get("SZ", 19))
    black = [coord(v) for v in d.get("AB", [])]
    white = [coord(v) for v in d.get("AW", [])]
    sol = d.get("SOL", [])
    if not sol:
        warnings.append(f"{path}: no SOL, skipped")
        return None
    to_play = sol[0][0]
    tree_roots = []
    for e in sol:
        node = MTree(coord(e[1]), e[0])
        node.status = 1                       # every alternative is RIGHT
        tree_roots.append(node)
    return pack_from_parts(path, sz, black, white, tree_roots, to_play, warnings)

def pack_from_parts(path, sz, black, white, tree_roots, to_play, warnings):
    # bounding box over stones + moves
    pts = [p for p in black + white + collect_moves(tree_roots) if p]
    if not pts:
        warnings.append(f"{path}: empty problem, skipped")
        return None
    black = [p for p in black if p]
    white = [p for p in white if p]
    cs = [p[0] for p in pts]; rs = [p[1] for p in pts]
    minc, maxc, minr, maxr = min(cs), max(cs), min(rs), max(rs)

    # normalize: mirror so the problem hugs the top-left
    flip_x = (minc + maxc) / 2 > (sz - 1) / 2
    flip_y = (minr + maxr) / 2 > (sz - 1) / 2
    if flip_x or flip_y:
        black = [transform(p, sz, flip_x, flip_y) for p in black]
        white = [transform(p, sz, flip_x, flip_y) for p in white]
        def tw(n):
            if n.move is not None:
                n.move = transform(n.move, sz, flip_x, flip_y)
            for ch in n.children: tw(ch)
        for r_ in tree_roots: tw(r_)
        cs = [p[0] for p in black + white + collect_moves(tree_roots)]
        rs = [p[1] for p in black + white + collect_moves(tree_roots)]
        minc, maxc, minr, maxr = min(cs), max(cs), min(rs), max(rs)

    # pad 2 on open sides, clamp to board
    MARGIN = 2
    x0 = 0 if minc <= MARGIN else minc - MARGIN
    y0 = 0 if minr <= MARGIN else minr - MARGIN
    x1 = min(sz - 1, maxc + MARGIN)
    y1 = min(sz - 1, maxr + MARGIN)
    w, h = x1 - x0 + 1, y1 - y0 + 1
    if w * h > 252:
        warnings.append(f"{path}: region {w}x{h} too large, skipped")
        return None

    flags = (1 if to_play == "W" else 0)
    if x0 == 0:      flags |= 1 << 1
    if y0 == 0:      flags |= 1 << 2
    if x1 == sz - 1: flags |= 1 << 3
    if y1 == sz - 1: flags |= 1 << 4

    def pos(p):
        c, r = p
        return (r - y0) * w + (c - x0)

    blob = bytearray()
    blob.append(flags)
    blob += bytes((w, h))
    blob.append(len(black)); blob += bytes(pos(p) for p in black)
    blob.append(len(white)); blob += bytes(pos(p) for p in white)

    def emit(node):
        blob.append(PASS if node.move is None else pos(node.move))
        blob.append(node.status)
        for ch in node.children:
            emit(ch)
        blob.append(POP)

    for r_ in tree_roots:
        emit(r_)
    blob.append(END_TREE)
    return bytes(blob)

# ---------------------------------------------------------------- commands

def gather(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += sorted(glob.glob(os.path.join(p, "**", "*.sgf"),
                                      recursive=True))
            files += sorted(glob.glob(os.path.join(p, "**", "*.json"),
                                      recursive=True))
        else:
            files += sorted(glob.glob(p)) if any(ch in p for ch in "*?") else [p]
    return files

def cmd_pack(out, paths):
    files = gather(paths)
    blobs, warnings = [], []
    for f in files:
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
            b = (pack_json if f.lower().endswith(".json")
                 else pack_sgf)(f, text, warnings)
            if b: blobs.append(b)
        except Exception as e:
            warnings.append(f"{f}: parse error: {e}")
    header = b"TSU1" + struct.pack("<H", len(blobs))
    off = len(header) + 4 * len(blobs)
    offsets = []
    for b in blobs:
        offsets.append(off); off += len(b)
    with open(out, "wb") as fh:
        fh.write(header)
        for o in offsets: fh.write(struct.pack("<I", o))
        for b in blobs: fh.write(b)
    for wmsg in warnings: print("  !", wmsg)
    total = os.path.getsize(out)
    print(f"packed {len(blobs)}/{len(files)} problems -> {out} "
          f"({total} bytes, avg {total // max(1, len(blobs))} B/problem)")

def cmd_dump(binfile, index):
    data = open(binfile, "rb").read()
    assert data[:4] == b"TSU1"
    count = struct.unpack_from("<H", data, 4)[0]
    print(f"{binfile}: {count} problems")
    if index is None:
        return
    off = struct.unpack_from("<I", data, 6 + 4 * index)[0]
    p = off
    flags = data[p]; p += 1
    w, h = data[p], data[p + 1]; p += 2
    board = [["." for _ in range(w)] for _ in range(h)]
    nb = data[p]; p += 1
    for i in range(nb):
        q = data[p + i]; board[q // w][q % w] = "X"
    p += nb
    nw = data[p]; p += 1
    for i in range(nw):
        q = data[p + i]; board[q // w][q % w] = "O"
    p += nw
    walls = "".join(s for b, s in ((1, "L"), (2, "T"), (3, "R"), (4, "B"))
                    if flags >> b & 1)
    print(f"problem {index}: {'white' if flags & 1 else 'black'} to play, "
          f"{w}x{h}, walls={walls or 'none'}")
    for row in board:
        print("   " + " ".join(row))
    depth = 0
    while data[p] != END_TREE:
        if data[p] == POP:
            depth -= 1; p += 1; continue
        q, st = data[p], data[p + 1]; p += 2
        mv = "pass" if q == PASS else f"({q % w},{q // w})"
        tag = {0: "", 1: "  <-- RIGHT", 2: "  (wrong)"}[st]
        print("   " + "  " * depth + mv + tag)
        depth += 1
    print(f"blob size: {p + 1 - off} bytes")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    if sys.argv[1] == "pack":
        cmd_pack(sys.argv[2], sys.argv[3:])
    elif sys.argv[1] == "dump":
        cmd_dump(sys.argv[2],
                 int(sys.argv[3]) if len(sys.argv) > 3 else None)
    else:
        print(__doc__); sys.exit(1)
