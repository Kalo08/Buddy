"""Give every top-side GND pad a short stub + via down to the B.Cu GND plane.

Run after router.py:  python gnd_fanout.py geom.json   -> gnd.json
"""
import json
import math

import numpy as np

import router as R

routes = json.load(open("routes.json"))
for l, xa, ya, xb, yb, w, net in routes["tracks"]:
    (j0, j1, i0, i1), m = R.raster_segment(xa, ya, xb, yb, w / 2)
    R.track_owner[l, j0:j1, i0:i1][m] = R.net_id[net]
for x, y, d, dr, net in routes["vias"]:
    (j0, j1, i0, i1), m = R.raster_segment(x, y, x, y, d / 2)
    R.track_owner[:, j0:j1, i0:i1][:, m] = R.net_id[net]

gid = R.net_id["GND"]
VD, VDR, TW = 0.6, 0.3, 0.3
copper = np.where(R.pad_owner != 0, R.pad_owner, R.track_owner)
foreign = (copper != 0) & (copper != gid)
r_t = TW / 2 + R.CLR + R.MARGIN
r_v = VD / 2 + R.CLR + R.MARGIN
blk_t = R.dilate(foreign[0] | R.hard, r_t)
via_ok = ~(R.dilate(R.pad_any[0] | R.pad_any[1] | R.hard, r_v) |
           R.dilate(foreign[0] | foreign[1], r_v))

out = {"tracks": [], "vias": [], "skipped": []}
placed = []
for key, pin in R.pins.items():
    if pin["net"] != "GND":
        continue
    p = pin["pads"][0]
    if "B" in p["layers"]:
        continue                       # THT / thermal-via pads already reach B.Cu
    px, py = p["x"], p["y"]
    best = None
    for rr in np.arange(0.6, 2.6, 0.1):
        for a in range(0, 360, 15):
            vx = px + rr * math.cos(math.radians(a)); vy = py + rr * math.sin(math.radians(a))
            j, i = R.cell(vx, vy)
            if not (0 <= j < R.H and 0 <= i < R.W) or not via_ok[j, i]:
                continue
            if any(math.hypot(vx - qx, vy - qy) < VD + 0.3 for qx, qy in placed):
                continue
            if R.seg_ok(0, px, py, vx, vy, np.stack([blk_t, blk_t]), gid, copper):
                best = (vx, vy)
                break
        if best:
            break
    if best is None:
        out["skipped"].append(list(key))
        continue
    vx, vy = round(best[0], 3), round(best[1], 3)
    placed.append((vx, vy))
    out["tracks"].append([0, px, py, vx, vy, TW, "GND"])
    out["vias"].append([vx, vy, VD, VDR, "GND"])
json.dump(out, open("gnd.json", "w"))
print(f"GND fan-out: {len(out['vias'])} vias, skipped {len(out['skipped'])}: {out['skipped']}")
