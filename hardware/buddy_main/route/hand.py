"""Place hand-designed 45/90 degree tracks in the live editor. Usage: edit JOBS, run with KiCad's python."""
import json
import math
import sys

sys.path.insert(0, r"C:\Users\Gavin\Downloads\kipy-site")
from kipy import KiCad
from kipy.board_types import BoardLayer, Track
from kipy.geometry import Vector2
from kipy.util.units import from_mm

LAYER = {"F": BoardLayer.BL_F_Cu, "B": BoardLayer.BL_B_Cu}


def push(jobs, label):
    board = KiCad(timeout_ms=120000).get_board()
    nets = {n.name: n for n in board.get_nets()}
    new = []
    for layer, pts, net, width in jobs:
        for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
            a = math.degrees(math.atan2(yb - ya, xb - xa)) % 45
            assert min(a, 45 - a) < 0.01, f"not 45/90: {(xa, ya)}->{(xb, yb)}"
            t = Track()
            t.start = Vector2.from_xy(from_mm(xa), from_mm(ya))
            t.end = Vector2.from_xy(from_mm(xb), from_mm(yb))
            t.width = from_mm(width)
            t.layer = LAYER[layer]
            t.net = nets[net]
            new.append(t)
    c = board.begin_commit()
    try:
        board.create_items(new)
        board.push_commit(c, label)
    except Exception:
        board.drop_commit(c)
        raise
    board.save()
    print("placed", len(new), "track segments, saved")


if __name__ == "__main__":
    push(json.load(open(sys.argv[1])), sys.argv[2] if len(sys.argv) > 2 else "Hand-routed tracks")
