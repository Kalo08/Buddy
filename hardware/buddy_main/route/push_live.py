"""Sync routes.json into the board open in KiCad's PCB Editor (via kicad-python).

Diffs routes.json (plus the plane fan-out) against the tracks/vias on the live
board, removes what is gone and adds what is new, all in one undo step.
Run with KiCad's python:  python.exe push_live.py [--dry]
"""
import json
import sys

sys.path.insert(0, r"C:\Users\Gavin\Downloads\kipy-site")
from kipy import KiCad
from kipy.board_types import BoardLayer, Track, Via
from kipy.geometry import Vector2
from kipy.util.units import from_mm, to_mm

dry = "--dry" in sys.argv
routes = json.load(open("routes.json"))
board = KiCad().get_board()
nets = {n.name: n for n in board.get_nets()}
LAYER = {0: BoardLayer.BL_F_Cu, 1: BoardLayer.BL_B_Cu}


def q(v):
    return int(round(v * 1e4))     # 0.1 um buckets


def track_key(layer, xa, ya, xb, yb, w):
    a, b = (q(xa), q(ya)), (q(xb), q(yb))
    return (layer,) + (a + b if a <= b else b + a) + (q(w),)


def split_t_junctions(tracks):
    """KiCad joins wires only at endpoints: split a wire wherever another wire of the same net ends on it."""
    out = [list(t) for t in tracks]
    changed = True
    while changed:
        changed = False
        ends = {}
        for t in out:
            ends.setdefault((t[6], t[0]), set()).update([(round(t[1], 3), round(t[2], 3)), (round(t[3], 3), round(t[4], 3))])
        for idx, t in enumerate(out):
            l, xa, ya, xb, yb, w, net = t
            for (px, py) in ends[(net, l)]:
                if (abs(px - xa) < 5e-4 and abs(py - ya) < 5e-4) or (abs(px - xb) < 5e-4 and abs(py - yb) < 5e-4):
                    continue
                dx, dy = xb - xa, yb - ya
                L2 = dx * dx + dy * dy
                if L2 < 1e-9:
                    continue
                u = ((px - xa) * dx + (py - ya) * dy) / L2
                if 0 < u < 1 and abs(xa + u * dx - px) < 2e-3 and abs(ya + u * dy - py) < 2e-3:
                    out[idx] = [l, xa, ya, px, py, w, net]
                    out.append([l, px, py, xb, yb, w, net])
                    changed = True
                    break
            if changed:
                break
    return out



want_t = {}
for l, xa, ya, xb, yb, w, net in split_t_junctions(routes["tracks"]):
    want_t[track_key(l, xa, ya, xb, yb, w)] = (l, xa, ya, xb, yb, w, net)
want_v = {}
for x, y, d, dr, net in routes["vias"]:
    want_v[(q(x), q(y), q(d))] = (x, y, d, dr, net)

live_t, live_v = {}, {}
for t in board.get_tracks():
    l = 0 if t.layer == BoardLayer.BL_F_Cu else 1 if t.layer == BoardLayer.BL_B_Cu else None
    if l is None:
        continue
    k = track_key(l, to_mm(t.start.x), to_mm(t.start.y), to_mm(t.end.x), to_mm(t.end.y), to_mm(t.width))
    live_t[k] = t
for v in board.get_vias():
    live_v[(q(to_mm(v.position.x)), q(to_mm(v.position.y)), q(to_mm(v.diameter)))] = v

rm = [live_t[k] for k in live_t if k not in want_t] + [live_v[k] for k in live_v if k not in want_v]
add_t = [want_t[k] for k in want_t if k not in live_t]
add_v = [want_v[k] for k in want_v if k not in live_v]
print(f"live: {len(live_t)} tracks, {len(live_v)} vias | remove {len(rm)}, add {len(add_t)} tracks + {len(add_v)} vias")
if dry:
    sys.exit(0)

new = []
for l, xa, ya, xb, yb, w, net in add_t:
    t = Track()
    t.start = Vector2.from_xy(from_mm(xa), from_mm(ya))
    t.end = Vector2.from_xy(from_mm(xb), from_mm(yb))
    t.width = from_mm(w)
    t.layer = LAYER[l]
    t.net = nets[net]
    new.append(t)
for x, y, d, dr, net in add_v:
    v = Via()
    v.position = Vector2.from_xy(from_mm(x), from_mm(y))
    v.diameter = from_mm(d)
    v.drill_diameter = from_mm(dr)
    v.net = nets[net]
    new.append(v)

commit = board.begin_commit()
try:
    if rm:
        board.remove_items(rm)
    if new:
        board.create_items(new)
    board.push_commit(commit, "Route remaining connections")
except Exception:
    board.drop_commit(commit)
    raise
board.refill_zones(block=True)
print("done; zones refilled")
