#!/usr/bin/env python3
"""
chess_pack.py - Convert Lichess puzzle CSV into a compact binary format
(chess.bin) for the Watchy puzzle app. Requires: pip install chess

Usage:
  python3 chess_pack.py pack chess.bin puzzles.csv [--min 600] [--max-rating 2000]
                        [--count 5000] [--theme mateIn2] [--max-plies 12]
  python3 chess_pack.py dump chess.bin [index]

Lichess convention: the CSV FEN is the position BEFORE the opponent's
last move; Moves[0] is that opponent move. The packer pre-applies it,
stores its squares for the last-move marker, and stores the remaining
line. Player moves are plies 0, 2, 4... of the stored line.

Binary format (little-endian):
  "CHP1"  u16 count  u8 nsets  [u8 len + name + u16 start]*  u32 offsets  blobs

Blob:
  flags   u8    bit0: player is black
  lastFrom, lastTo  u8, u8      (opponent's pre-applied move; marker)
  rating  u16
  board   32 bytes, nibble per square, sq0=a1..sq63=h8
          0 empty; 1..6 = white P N B R Q K; 7..12 = black
  nMoves  u8
  moves   nMoves * (from u8, to u8, promo u8: 0 none / 2 N 3 B 4 R 5 Q)

Every puzzle is verified at pack time: a mirror of the on-device move
application is compared to python-chess after every ply.
"""

import sys, csv, struct, argparse

try:
    import chess
except ImportError:
    sys.exit("needs python-chess:  pip install chess")

PIECE_NIBBLE = {  # (piece_type, color) -> nibble
    (chess.PAWN, True): 1, (chess.KNIGHT, True): 2, (chess.BISHOP, True): 3,
    (chess.ROOK, True): 4, (chess.QUEEN, True): 5, (chess.KING, True): 6,
}
PROMO_CODE = {chess.KNIGHT: 2, chess.BISHOP: 3, chess.ROOK: 4, chess.QUEEN: 5}

def board_nibbles(board):
    bd = [0] * 64
    for sq, pc in board.piece_map().items():
        n = PIECE_NIBBLE[(pc.piece_type, True)]
        bd[sq] = n if pc.color == chess.WHITE else n + 6
    return bd

def device_apply(bd, frm, to, promo):
    """Mirror of the on-device move application. Mutates bd."""
    p = bd[frm]
    if p in (6, 12) and abs(frm % 8 - to % 8) == 2:      # castling
        rank = frm - frm % 8
        if to > frm:
            bd[rank + 5] = bd[rank + 7]; bd[rank + 7] = 0
        else:
            bd[rank + 3] = bd[rank + 0]; bd[rank + 0] = 0
    if p in (1, 7) and frm % 8 != to % 8 and bd[to] == 0:  # en passant
        bd[to + (-8 if p == 1 else 8)] = 0
    bd[to] = p if not promo else (promo if p <= 6 else promo + 6)
    bd[frm] = 0

def pack_puzzle(row, warnings, max_plies):
    fen, moves = row["FEN"], row["Moves"].split()
    rating = int(row["Rating"])
    board = chess.Board(fen)
    setup_mv = chess.Move.from_uci(moves[0])
    if setup_mv not in board.legal_moves:
        warnings.append(f"{row['PuzzleId']}: illegal setup move, skipped")
        return None
    board.push(setup_mv)
    line = moves[1:]
    if not line or len(line) > max_plies:
        return None

    bd = board_nibbles(board)
    verify = chess.Board(fen); verify.push(setup_mv)

    enc = []
    for uci in line:
        mv = chess.Move.from_uci(uci)
        if mv not in verify.legal_moves:
            warnings.append(f"{row['PuzzleId']}: illegal line move {uci}, skipped")
            return None
        promo = PROMO_CODE.get(mv.promotion, 0)
        enc.append((mv.from_square, mv.to_square, promo))
        device_apply(bd, mv.from_square, mv.to_square, promo)
        verify.push(mv)
        if bd != board_nibbles(verify):
            warnings.append(f"{row['PuzzleId']}: DEVICE LOGIC MISMATCH at {uci}")
            return None

    # re-derive the starting board (device_apply mutated bd)
    start = board_nibbles(board)
    blob = bytearray()
    blob.append(1 if board.turn == chess.BLACK else 0)
    blob.append(setup_mv.from_square)
    blob.append(setup_mv.to_square)
    blob += struct.pack("<H", min(rating, 65535))
    for i in range(0, 64, 2):
        blob.append(start[i] | (start[i + 1] << 4))
    blob.append(len(line))
    for f, t, p in enc:
        blob += bytes((f, t, p))
    return rating, bytes(blob)

def cmd_pack(args):
    warnings, packed = [], []
    with open(args.csv, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            r = int(row["Rating"])
            if r < args.min or r > args.max_rating:
                continue
            if args.theme and args.theme not in row.get("Themes", ""):
                continue
            out = pack_puzzle(row, warnings, args.max_plies)
            if out:
                packed.append(out)
            if args.count and len(packed) >= args.count * 2:
                break  # gather extra, trim after sort
    packed.sort(key=lambda x: x[0])
    if args.count:
        # trim evenly across the rating range, not just the top
        step = max(1, len(packed) // args.count)
        packed = packed[::step][:args.count]

    sets, blobs = [], []
    for rating, blob in packed:
        band = rating // 300 * 300
        name = f"{band}-{band + 299}"
        if not sets or sets[-1][0] != name:
            if len(sets) < 32:
                sets.append((name, len(blobs)))
        blobs.append(blob)

    header = b"CHP1" + struct.pack("<H", len(blobs)) + bytes([len(sets)])
    for name, start in sets:
        nb = name.encode()[:24]
        header += bytes([len(nb)]) + nb + struct.pack("<H", start)
    off = len(header) + 4 * len(blobs)
    offsets = []
    for b in blobs:
        offsets.append(off); off += len(b)
    with open(args.out, "wb") as fh:
        fh.write(header)
        for o in offsets: fh.write(struct.pack("<I", o))
        for b in blobs: fh.write(b)
    for w in warnings[:20]: print("  !", w)
    if len(warnings) > 20: print(f"  ! (+{len(warnings)-20} more)")
    import os
    print(f"packed {len(blobs)} puzzles, {len(sets)} rating bands -> "
          f"{args.out} ({os.path.getsize(args.out)} bytes)")
    for name, start in sets:
        print(f"    {start:5d}  {name}")

GLYPH = " PNBRQKpnbrqk"

def cmd_dump(binfile, index):
    data = open(binfile, "rb").read()
    assert data[:4] == b"CHP1"
    count = struct.unpack_from("<H", data, 4)[0]
    p = 6; nsets = data[p]; p += 1
    sets = []
    for _ in range(nsets):
        ln = data[p]; p += 1
        sets.append((data[p:p+ln].decode(),
                     struct.unpack_from("<H", data, p+ln)[0]))
        p += ln + 2
    print(f"{binfile}: {count} puzzles, bands: {sets}")
    if index is None: return
    off = struct.unpack_from("<I", data, p + 4*index)[0]
    q = off
    flags = data[q]; lf, lt = data[q+1], data[q+2]
    rating = struct.unpack_from("<H", data, q+3)[0]
    q += 5
    bd = []
    for i in range(32):
        bd.append(data[q+i] & 0xF); bd.append(data[q+i] >> 4)
    q += 32
    n = data[q]; q += 1
    print(f"puzzle {index}: rating {rating}, "
          f"{'black' if flags & 1 else 'white'} to move, "
          f"opponent just played {chess.square_name(lf)}{chess.square_name(lt)}")
    for rank in range(7, -1, -1):
        print("   " + " ".join(GLYPH[bd[rank*8 + f]] if bd[rank*8+f] else "."
                               for f in range(8)))
    for i in range(n):
        f, t, pr = data[q+3*i:q+3*i+3]
        who = "YOU" if i % 2 == 0 else "opp"
        print(f"   {who}: {chess.square_name(f)}{chess.square_name(t)}"
              + (f" ={GLYPH[pr]}" if pr else ""))

if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "dump":
        cmd_dump(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else None)
    else:
        ap = argparse.ArgumentParser()
        ap.add_argument("cmd", choices=["pack"])
        ap.add_argument("out")
        ap.add_argument("csv")
        ap.add_argument("--min", type=int, default=0)
        ap.add_argument("--max-rating", type=int, default=9999)
        ap.add_argument("--count", type=int, default=0)
        ap.add_argument("--theme", default="")
        ap.add_argument("--max-plies", type=int, default=12)
        cmd_pack(ap.parse_args())
