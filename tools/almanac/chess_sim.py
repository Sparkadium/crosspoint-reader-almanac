"""Mirror of the on-device chess flow, validated against python-chess."""
import struct, sys, chess

def read_bin(path):
    d = open(path, "rb").read()
    assert d[:4] == b"CHP1"
    count = struct.unpack_from("<H", d, 4)[0]
    p = 6; nsets = d[p]; p += 1
    for _ in range(nsets): p += 1 + d[p] + 2
    return d, count, p

class Dev:
    def __init__(self, data, base, idx):
        off = struct.unpack_from("<I", data, base + 4*idx)[0]
        q = off
        self.playerBlack = bool(data[q] & 1)
        self.dispFrom, self.dispTo = data[q+1], data[q+2]
        self.rating = struct.unpack_from("<H", data, q+3)[0]
        q += 5
        self.setup = []
        for i in range(32):
            self.setup.append(data[q+i] & 0xF); self.setup.append(data[q+i] >> 4)
        q += 32
        self.lineN = data[q]; q += 1
        self.line = [tuple(data[q+3*i:q+3*i+3]) for i in range(self.lineN)]
        self.reset()

    def reset(self):
        self.bd = list(self.setup); self.linePos = 0; self.mode = "PLAYING"

    def apply(self, frm, to, promo):
        p = self.bd[frm]
        if p in (6, 12) and abs(frm % 8 - to % 8) == 2:
            rank = frm - frm % 8
            if to > frm: self.bd[rank+5] = self.bd[rank+7]; self.bd[rank+7] = 0
            else:        self.bd[rank+3] = self.bd[rank+0]; self.bd[rank+0] = 0
        if p in (1, 7) and frm % 8 != to % 8 and self.bd[to] == 0:
            self.bd[to + (-8 if p == 1 else 8)] = 0
        self.bd[to] = (promo if p <= 6 else promo + 6) if promo else p
        self.bd[frm] = 0

    def attempt(self, sel, to):
        f, t, pr = self.line[self.linePos]
        if sel == f and to == t:
            self.apply(f, t, pr); self.linePos += 1
            if self.linePos >= self.lineN: self.mode = "SOLVED"; return
            self.apply(*self.line[self.linePos]); self.linePos += 1
            if self.linePos >= self.lineN: self.mode = "SOLVED"
            return
        self.apply(sel, to, 0); self.mode = "OFFTREE"

    def replay(self):
        keep = self.linePos; self.reset()
        while self.linePos < keep:
            self.apply(*self.line[self.linePos]); self.linePos += 1

def to_sq(scr, black):
    r, c = scr // 8, scr % 8
    return r*8 + (7-c) if black else (7-r)*8 + c

def nibbles(board):
    m = {1:1,2:2,3:3,4:4,5:5,6:6}
    bd = [0]*64
    for sq, pc in board.piece_map().items():
        bd[sq] = m[pc.piece_type] + (0 if pc.color else 6)
    return bd

data, count, base = read_bin(sys.argv[1])
solved = wrong_ok = undo_ok = 0
for i in range(count):
    g = Dev(data, base, i)
    # winning line through the two-phase input path
    while g.mode == "PLAYING":
        f, t, pr = g.line[g.linePos]
        g.attempt(f, t)
    assert g.mode == "SOLVED", f"puzzle {i} not solved"
    solved += 1
    # wrong move then retry restores exactly
    g.reset()
    f, t, pr = g.line[0]
    wrong_to = next(sq for sq in range(64) if sq != t and g.bd[sq] == 0)
    before = list(g.bd)
    g.attempt(f, wrong_to)
    assert g.mode == "OFFTREE"
    g.replay()
    assert g.bd == before and g.mode == "PLAYING", f"puzzle {i} retry broken"
    wrong_ok += 1
    # undo after first exchange returns to start (when line is long enough)
    if g.lineN >= 3:
        g.attempt(*g.line[0][:2])
        g.linePos = g.linePos - 2 if g.linePos >= 2 else 0
        g.replay()
        assert g.bd == before, f"puzzle {i} undo broken"
    undo_ok += 1
# screen mapping round-trips for both orientations
for black in (False, True):
    seen = {to_sq(s, black) for s in range(64)}
    assert seen == set(range(64)), "toSq not a bijection"
print(f"{count} puzzles: {solved} solved via input path, "
      f"{wrong_ok} wrong+retry ok, {undo_ok} undo ok, toSq bijective")
