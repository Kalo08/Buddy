"""Small two-layer grid router for the Buddy main board.

Reads geom.json (from extract.py), routes every net except GND (which goes to
pours plus fan-out vias), and writes routes.json:
  {"tracks": [[layer, x1, y1, x2, y2, width, net], ...],
   "vias":   [[x, y, diameter, drill, net], ...],
   "failed": [[net, pin], ...]}

Method: A* on a 0.1 mm grid with octile moves, foreign copper inflated by
(track half-width + clearance), rip-up-and-reroute with history costs when a
connection is blocked, then any-angle string pulling of each path.
"""
import heapq
import itertools
import json
import math
import sys
import time

import numpy as np

RES = 0.1
X0, Y0 = 99.9, 99.9
CLR = 0.15            # board clearance rule
MARGIN = 0.07         # raster safety margin (half a cell diagonal)
EDGE_CLR = 0.5        # copper to board edge
HOLE_CLR = 0.25       # copper to NPTH hole
B_COST = 1.1          # slight preference for F.Cu
PLANES = {"GND": "In1.Cu", "3V3": "In2.Cu"}   # these nets drop to inner planes
VIA_COST = 14.0
NARROW = 0.2
POWER = {"VSYS": 0.5, "/Power and Protection/VBAT_IN": 0.5, "/Power and Protection/VBAT_F": 0.5,
         "/Power and Protection/Q_SRC": 0.5, "VBUS": 0.5, "/Power and Protection/VBUS_F": 0.5,
         "5V": 0.5, "3V3": 0.4, "/Power and Protection/SW_3V3": 0.5, "/Power and Protection/SW_5V": 0.5,
         "/Motor Drivers/M1_A": 0.5, "/Motor Drivers/M1_B": 0.5, "/Motor Drivers/M2_A": 0.5,
         "/Motor Drivers/M2_B": 0.5, "/Motor Drivers/M3_A": 0.5, "/Motor Drivers/M3_B": 0.5,
         "/Motor Drivers/AISEN1": 0.5, "/Motor Drivers/AISEN2": 0.5, "/Motor Drivers/BISEN1": 0.5}
VIA = {0.2: (0.6, 0.3), 0.4: (0.8, 0.4), 0.5: (0.8, 0.4)}
NECK_R = 1.3          # wide nets may neck down to NARROW this close to their pins

geom = json.load(open(sys.argv[1] if len(sys.argv) > 1 else "geom.json"))
x1o, y1o, x2o, y2o = geom["outline"]
W = int(round((x2o - X0) / RES)) + 3
H = int(round((y2o - Y0) / RES)) + 3
N = W * H
xs = X0 + np.arange(W) * RES
ys = Y0 + np.arange(H) * RES
GX, GY = np.meshgrid(xs, ys)


def cell(x, y):
    return int(round((y - Y0) / RES)), int(round((x - X0) / RES))


def disk(r):
    k = int(math.ceil(r / RES))
    return [(dy, dx) for dy in range(-k, k + 1) for dx in range(-k, k + 1) if (dx * dx + dy * dy) * RES * RES <= r * r]


def dilate(mask, r):
    out = mask.copy()
    for dy, dx in disk(r):
        if dy == 0 and dx == 0:
            continue
        sy0, sy1 = max(0, -dy), H - max(0, dy)
        sx0, sx1 = max(0, -dx), W - max(0, dx)
        out[sy0 + dy:sy1 + dy, sx0 + dx:sx1 + dx] |= mask[sy0:sy1, sx0:sx1]
    return out


# ---------------------------------------------------------------- nets / pins
net_names = geom["nets"]
net_id = {n: i + 1 for i, n in enumerate(net_names)}
uniq = itertools.count(10000)
pins = {}                    # (ref, num) -> dict
for p in geom["pads"]:
    if p["npth"] or not p["layers"]:
        continue
    key = (p["ref"], p["num"])
    pins.setdefault(key, {"net": p["net"], "pads": []})["pads"].append(p)

# static copper: pads
pad_owner = np.zeros((2, H, W), np.int32)
pad_any = np.zeros((2, H, W), bool)
hard = np.zeros((H, W), bool)          # edge + NPTH (both layers)
for p in geom["pads"]:
    l, t, r, b = p["bbox"]
    if p["npth"]:
        rr = p["drill"] / 2 + HOLE_CLR - CLR
        hard |= (GX - p["x"]) ** 2 + (GY - p["y"]) ** 2 <= rr * rr
        continue
    nid = net_id.get(p["net"]) if p["net"] and not p["net"].startswith("unconnected") else next(uniq)
    if p["circle"]:
        rr = (r - l) / 2
        m = (GX - p["x"]) ** 2 + (GY - p["y"]) ** 2 <= rr * rr
    else:
        m = (GX >= l) & (GX <= r) & (GY >= t) & (GY <= b)
    for li, ln in enumerate("FB"):
        if ln in p["layers"]:
            pad_owner[li][m] = nid
            pad_any[li] |= m
# board edge (rounded rectangle, r = 4 mm) shrunk by the edge clearance
rad = 4.0
ex1, ey1, ex2, ey2 = x1o + 0.025, y1o + 0.025, x2o - 0.025, y2o - 0.025
cx = np.clip(GX, ex1 + rad, ex2 - rad)
cy = np.clip(GY, ey1 + rad, ey2 - rad)
inside = ((GX - cx) ** 2 + (GY - cy) ** 2 <= (rad - (EDGE_CLR - CLR)) ** 2) & \
         (GX >= ex1 + EDGE_CLR - CLR) & (GX <= ex2 - EDGE_CLR + CLR) & \
         (GY >= ey1 + EDGE_CLR - CLR) & (GY <= ey2 - EDGE_CLR + CLR)
hard |= ~inside

# ---------------------------------------------------------------- route store
routes = {}          # conn key -> {"net", "tracks": [(l,x1,y1,x2,y2,w)], "vias": [(x,y,d,dr)]}
track_owner = np.zeros((2, H, W), np.int32)
conn_owner = np.full((2, H, W), -1, np.int32)
conn_index = {}
history = np.zeros((2, H, W), np.float32)


def raster_segment(x1, y1, x2, y2, hw):
    """Boolean mask of cells whose centre is within hw of the segment, plus bbox."""
    pad = hw + RES
    i0 = max(0, int((min(x1, x2) - pad - X0) / RES)); i1 = min(W, int((max(x1, x2) + pad - X0) / RES) + 2)
    j0 = max(0, int((min(y1, y2) - pad - Y0) / RES)); j1 = min(H, int((max(y1, y2) + pad - Y0) / RES) + 2)
    gx, gy = GX[j0:j1, i0:i1], GY[j0:j1, i0:i1]
    dx, dy = x2 - x1, y2 - y1
    L2 = dx * dx + dy * dy
    if L2 < 1e-12:
        d2 = (gx - x1) ** 2 + (gy - y1) ** 2
    else:
        t = np.clip(((gx - x1) * dx + (gy - y1) * dy) / L2, 0, 1)
        d2 = (gx - x1 - t * dx) ** 2 + (gy - y1 - t * dy) ** 2
    return (j0, j1, i0, i1), d2 <= hw * hw


def paint(key, value_net, value_conn):
    r = routes[key]
    for l, xa, ya, xb, yb, w in r["tracks"]:
        (j0, j1, i0, i1), m = raster_segment(xa, ya, xb, yb, w / 2)
        track_owner[l, j0:j1, i0:i1][m] = value_net
        conn_owner[l, j0:j1, i0:i1][m] = value_conn
    for x, y, d, dr in r["vias"]:
        (j0, j1, i0, i1), m = raster_segment(x, y, x, y, d / 2)
        for l in (0, 1):
            track_owner[l, j0:j1, i0:i1][m] = value_net
            conn_owner[l, j0:j1, i0:i1][m] = value_conn


def rebuild():
    track_owner[:] = 0
    conn_owner[:] = -1
    for k in routes:
        paint(k, net_id[routes[k]["net"]], conn_index[k])


# ---------------------------------------------------------------- connections
def pin_center(key):
    p = pins[key]["pads"][0]
    return p["x"], p["y"]


conns = []           # (net, pin key) ; first pin of each net is the seed
seeds = {}
for net in net_names:
    if net in PLANES or net.startswith("unconnected"):
        continue
    keys = [k for k, v in pins.items() if v["net"] == net]
    if len(keys) < 2:
        continue
    # Prim order from the pin closest to the net's centroid
    pts = {k: pin_center(k) for k in keys}
    mx = sum(p[0] for p in pts.values()) / len(pts); my = sum(p[1] for p in pts.values()) / len(pts)
    start = min(keys, key=lambda k: (pts[k][0] - mx) ** 2 + (pts[k][1] - my) ** 2)
    seeds[net] = start
    done, rest = [start], set(keys) - {start}
    total = 0.0
    while rest:
        best = min(((math.dist(pts[a], pts[b]), a, b) for a in done for b in rest))
        total += best[0]
        conns.append((net, best[2], best[1]))
        done.append(best[2]); rest.discard(best[2])
order_len = {}
for net, k, _ in conns:
    order_len[net] = order_len.get(net, 0) + 1
net_len = {}
for net, k, a in conns:
    net_len[net] = net_len.get(net, 0.0) + math.dist(pin_center(k), pin_center(a))
conns.sort(key=lambda c: (0 if c[0] in POWER else 1, net_len[c[0]]))
for i, c in enumerate(conns):
    conn_index[(c[0], c[1])] = i


def pin_cells(key, layer_mask):
    """Cells of this pin's pads on each layer (list of flat state ids)."""
    out = []
    for p in pins[key]["pads"]:
        l, t, r, b = p["bbox"]
        j0, i0 = cell(l, t); j1, i1 = cell(r, b)
        for li, ln in enumerate("FB"):
            if ln not in p["layers"]:
                continue
            for j in range(max(0, j0), min(H, j1 + 1)):
                for i in range(max(0, i0), min(W, i1 + 1)):
                    if layer_mask[li][j, i]:
                        out.append(li * N + j * W + i)
            jc, ic = cell(p["x"], p["y"])
            out.append(li * N + jc * W + ic)
    return out


# ---------------------------------------------------------------- search
SQ2 = math.sqrt(2)
MOVES = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0),
         (1, 1, SQ2), (1, -1, SQ2), (-1, 1, SQ2), (-1, -1, SQ2)]


def astar(sources, targets, tx, ty, free, soft, viafree, window, pen):
    """free/soft/viafree are bytes per layer; soft==1 means passable only with penalty."""
    ci0, ci1, cj0, cj1 = window
    tset = set(targets)
    g = {}
    parent = {}
    heap = []
    for s in sources:
        g[s] = 0.0
        parent[s] = -1
        l, rem = divmod(s, N); j, i = divmod(rem, W)
        h = max(abs(i - tx), abs(j - ty)) + (SQ2 - 1) * min(abs(i - tx), abs(j - ty))
        heapq.heappush(heap, (h, 0.0, s))
    expanded = 0
    while heap:
        f, gc, s = heapq.heappop(heap)
        if gc > g.get(s, 1e18):
            continue
        if s in tset:
            path = []
            while s != -1:
                path.append(s); s = parent[s]
            return path[::-1], expanded
        expanded += 1
        if expanded > 900000:
            return None, expanded
        l, rem = divmod(s, N); j, i = divmod(rem, W)
        lc = B_COST if l else 1.0
        for di, dj, c in MOVES:
            ni, nj = i + di, j + dj
            if ni < ci0 or ni > ci1 or nj < cj0 or nj > cj1:
                continue
            ns = l * N + nj * W + ni
            if not free[ns] and ns not in tset:
                continue
            ng = gc + c * lc + (pen * (1.0 + hist_flat[ns]) if soft[ns] else 0.0) + hist_flat[ns] * 0.3
            if ng < g.get(ns, 1e18):
                g[ns] = ng; parent[ns] = s
                h = max(abs(ni - tx), abs(nj - ty)) + (SQ2 - 1) * min(abs(ni - tx), abs(nj - ty))
                heapq.heappush(heap, (ng + h, ng, ns))
        # via
        o = 1 - l
        ns = o * N + rem
        if viafree[rem] and (free[ns] or ns in tset):
            ng = gc + VIA_COST + (pen if soft[ns] else 0.0)
            if ng < g.get(ns, 1e18):
                g[ns] = ng; parent[ns] = s
                h = max(abs(i - tx), abs(j - ty)) + (SQ2 - 1) * min(abs(i - tx), abs(j - ty))
                heapq.heappush(heap, (ng + h, ng, ns))
    return None, expanded


def access_mask(net):
    """Cells a narrow track may use inside/along this net's own pads.

    Fine-pitch pads (0.5 mm) leave no grid cell outside the inflated
    neighbours, so a track may run along the pad's centre line, kept within the
    pad's own width, and continue 0.45 mm past each end of the pad.
    """
    acc = np.zeros((2, H, W), bool)
    for k, v in pins.items():
        if v["net"] != net:
            continue
        for p in v["pads"]:
            l, t, r, b = p["bbox"]
            w, h = r - l, b - t
            half = max(0.0, min(w, h) / 2 - NARROW / 2) + 0.0505
            if w <= h:
                m = (np.abs(GX - p["x"]) <= half) & (GY >= t - 0.45) & (GY <= b + 0.45)
            else:
                m = (np.abs(GY - p["y"]) <= half) & (GX >= l - 0.45) & (GX <= r + 0.45)
            for li, ln in enumerate("FB"):
                if ln in p["layers"]:
                    acc[li] |= m
    return acc & ~np.stack([hard, hard])


def masks_for(net, width):
    nid = net_id[net]
    copper = np.where(pad_owner != 0, pad_owner, track_owner)
    static_foreign = ((pad_owner != 0) & (pad_owner != nid))
    track_foreign = ((track_owner != 0) & (track_owner != nid) & (pad_owner == 0))
    r_n = NARROW / 2 + CLR + MARGIN
    r_w = width / 2 + CLR + MARGIN
    vd, _ = VIA[width]
    r_v = vd / 2 + CLR + MARGIN
    hard_n = np.stack([dilate(static_foreign[l] | hard, r_n) for l in (0, 1)])
    soft_n = np.stack([dilate(track_foreign[l], r_n) for l in (0, 1)])
    if width > NARROW:
        hard_w = np.stack([dilate(static_foreign[l] | hard, r_w) for l in (0, 1)])
        soft_w = np.stack([dilate(track_foreign[l], r_w) for l in (0, 1)])
        near = np.zeros((H, W), bool)
        for k, v in pins.items():
            if v["net"] == net:
                x, y = pin_center(k)
                near |= (GX - x) ** 2 + (GY - y) ** 2 <= NECK_R ** 2
        near = np.stack([near, near])
        hard_c = np.where(near, hard_n, hard_w)
        soft_c = np.where(near, soft_n, soft_w)
    else:
        hard_w, soft_w, hard_c, soft_c = hard_n, soft_n, hard_n, soft_n
    acc = access_mask(net)
    hard_c = hard_c & ~acc
    soft_c = soft_c & ~acc
    hard_n = hard_n & ~acc
    # vias: keep off every pad (any net) and off foreign tracks
    via_block = dilate(pad_any[0] | pad_any[1] | hard, r_v) | \
        dilate((static_foreign[0] | static_foreign[1]), r_v)
    via_soft = dilate(track_foreign[0] | track_foreign[1], r_v)
    return dict(hard_c=hard_c, soft_c=soft_c, hard_w=hard_w, soft_w=soft_w,
                via_block=via_block, via_soft=via_soft, copper=copper, nid=nid)


def seg_ok(l, xa, ya, xb, yb, blocked, nid, copper):
    L = math.hypot(xb - xa, yb - ya)
    n = max(1, int(L / (RES * 0.5)))
    for t in range(n + 1):
        x = xa + (xb - xa) * t / n; y = ya + (yb - ya) * t / n
        j, i = cell(x, y)
        if blocked[l, j, i] and copper[l, j, i] != nid:
            return False
    return True


def snap_start(s, net, m):
    """Exact start point for a path that begins on existing net copper."""
    l, rem = divmod(s, N); j, i = divmod(rem, W)
    x, y = xs[i], ys[j]
    best = None
    for k, v in pins.items():
        if v["net"] != net:
            continue
        for p in v["pads"]:
            lft, t, r, b = p["bbox"]
            if "FB"[l] in p["layers"] and lft - RES <= x <= r + RES and t - RES <= y <= b + RES:
                return p["x"], p["y"]
    for key, rt in routes.items():
        if rt["net"] != net:
            continue
        for tl, xa, ya, xb, yb, w in rt["tracks"]:
            if tl != l:
                continue
            dx, dy = xb - xa, yb - ya
            L2 = dx * dx + dy * dy or 1e-12
            t = max(0, min(1, ((x - xa) * dx + (y - ya) * dy) / L2))
            px, py = xa + t * dx, ya + t * dy
            d = math.hypot(px - x, py - y)
            if best is None or d < best[0]:
                best = (d, px, py)
        for vx, vy, d, dr in rt["vias"]:
            dd = math.hypot(vx - x, vy - y)
            if best is None or dd < best[0]:
                best = (dd, vx, vy)
    return (best[1], best[2]) if best and best[0] < 0.6 else (x, y)


def build_geometry(path, net, width, target_key, m):
    pts = []
    for s in path:
        l, rem = divmod(s, N); j, i = divmod(rem, W)
        pts.append((l, xs[i], ys[j]))
    sx, sy = snap_start(path[0], net, m)
    pts[0] = (pts[0][0], sx, sy)
    tx, ty = pin_center(target_key)
    pts[-1] = (pts[-1][0], tx, ty)
    blocked_c = m["hard_c"] | m["soft_c"]
    blocked_w = m["hard_w"] | m["soft_w"]
    nid, copper = m["nid"], m["copper"]
    tracks, vias = [], []
    vd, vdr = VIA[width]
    # split into same-layer runs
    runs, cur = [], [pts[0]]
    for p in pts[1:]:
        if p[0] != cur[-1][0]:
            runs.append(cur); vias.append((cur[-1][1], cur[-1][2], vd, vdr))
            cur = [(p[0], cur[-1][1], cur[-1][2])]
            if (p[1], p[2]) != (cur[0][1], cur[0][2]):
                cur.append(p)
        else:
            cur.append(p)
    runs.append(cur)
    for run in runs:
        if len(run) < 2:
            continue
        l = run[0][0]
        i = 0
        while i < len(run) - 1:
            # gallop then binary search for the farthest visible point
            step, last_ok = 1, i + 1
            while i + step < len(run) and seg_ok(l, run[i][1], run[i][2], run[i + step][1], run[i + step][2], blocked_c, nid, copper):
                last_ok = i + step; step *= 2
            lo, hi = last_ok, min(len(run) - 1, i + step)
            while hi - lo > 1:
                mid = (lo + hi) // 2
                if seg_ok(l, run[i][1], run[i][2], run[mid][1], run[mid][2], blocked_c, nid, copper):
                    lo = mid
                else:
                    hi = mid
            j = lo
            xa, ya, xb, yb = run[i][1], run[i][2], run[j][1], run[j][2]
            if math.hypot(xb - xa, yb - ya) > 1e-6:
                w = width if (width > NARROW and seg_ok(l, xa, ya, xb, yb, blocked_w, nid, copper)) else NARROW
                tracks.append((l, round(xa, 4), round(ya, 4), round(xb, 4), round(yb, 4), w))
            i = j
    return tracks, [(round(x, 4), round(y, 4), d, dr) for x, y, d, dr in vias]


def route_conn(net, key, allow_soft, full_board=False):
    width = POWER.get(net, NARROW)
    m = masks_for(net, width)
    nid = m["nid"]
    free_l = []
    soft_l = []
    for l in (0, 1):
        f = ~m["hard_c"][l] if allow_soft else ~(m["hard_c"][l] | m["soft_c"][l])
        free_l.append(f); soft_l.append(m["soft_c"][l] & f)
    free = np.concatenate([free_l[0].ravel(), free_l[1].ravel()]).astype(np.uint8).tobytes()
    soft = np.concatenate([soft_l[0].ravel(), soft_l[1].ravel()]).astype(np.uint8).tobytes()
    vblock = m["via_block"] | (False if allow_soft else m["via_soft"])
    viafree = (~vblock).ravel().astype(np.uint8).tobytes()
    # sources: only copper already joined to this net's tree
    tree = np.argwhere(track_owner == nid)
    sources = [int(l * N + j * W + i) for l, j, i in tree if free_l[l][j, i]]
    for k in connected[net]:
        sources += pin_cells(k, free_l)
    targets = pin_cells(key, free_l)
    if not sources or not targets:
        return None
    tx, ty = cell(*pin_center(key))[::-1]
    if full_board:
        window = (0, W - 1, 0, H - 1)
    else:
        sj = [divmod(s % N, W) for s in sources]
        near = min(sj, key=lambda q: (q[1] - tx) ** 2 + (q[0] - ty) ** 2)
        mg = int(6 / RES)
        window = (max(0, min(near[1], tx) - mg), min(W - 1, max(near[1], tx) + mg),
                  max(0, min(near[0], ty) - mg), min(H - 1, max(near[0], ty) + mg))
    global hist_flat
    hist_flat = history.ravel().tolist()
    path, _ = astar(sources, targets, tx, ty, free, soft, viafree, window, pen=25.0)
    if path is None:
        return None
    tracks, vias = build_geometry(path, net, width, key, m)
    conflict_cells = [s for s in path if soft[s]]
    return tracks, vias, conflict_cells


hist_flat = None
connected = {net: {k} for net, k in seeds.items()}


def victims_of(cells, net):
    out = set()
    nid = net_id[net]
    for s in cells:
        l, rem = divmod(s, N); j, i = divmod(rem, W)
        win = conn_owner[l, max(0, j - 6):j + 7, max(0, i - 6):i + 7]
        nets_ = track_owner[l, max(0, j - 6):j + 7, max(0, i - 6):i + 7]
        for c in np.unique(win[(win >= 0) & (nets_ != nid)]):
            out.add(int(c))
    return out


fanout = {"tracks": [], "vias": [], "skipped": []}


def plane_fanout():
    """Stub + via from every SMD pad of a plane net down to its inner plane.

    The stubs and vias become static copper (pad_owner) so signal routing
    treats them like pads and never rips them up.
    """
    VD, VDR = 0.6, 0.3
    placed = []
    keys = [k for k, v in pins.items() if v["net"] in PLANES]
    # tightest pads first: smallest pad gets first pick of via sites
    keys.sort(key=lambda k: min(p["bbox"][2] - p["bbox"][0] for p in pins[k]["pads"]) *
              min(p["bbox"][3] - p["bbox"][1] for p in pins[k]["pads"]))
    for key in keys:
        net = pins[key]["net"]
        pads = pins[key]["pads"]
        if any(len(p["layers"]) == 2 for p in pads):
            continue                          # through-hole / via-in-pad already reaches the plane
        nid = net_id[net]
        p = pads[0]
        l = 0 if "F" in p["layers"] else 1
        px, py = p["x"], p["y"]
        copper = np.where(pad_owner != 0, pad_owner, track_owner)
        foreign = (copper != 0) & (copper != nid)
        acc = access_mask(net)
        tw = 0.3 if min(p["bbox"][2] - p["bbox"][0], p["bbox"][3] - p["bbox"][1]) >= 0.5 else NARROW
        blk = dilate(foreign[l] | hard, tw / 2 + CLR + MARGIN) & ~acc[l]
        r_v = VD / 2 + CLR + MARGIN
        via_bad = dilate(pad_any[0] | pad_any[1] | hard | foreign[0] | foreign[1], r_v)
        best = None
        for rr in np.arange(0.5, 3.01, 0.1):
            for a in range(0, 360, 10):
                vx = px + rr * math.cos(math.radians(a)); vy = py + rr * math.sin(math.radians(a))
                j, i = cell(vx, vy)
                if not (0 <= j < H and 0 <= i < W) or via_bad[j, i]:
                    continue
                if any(math.hypot(vx - qx, vy - qy) < VD + 0.35 for qx, qy in placed):
                    continue
                if seg_ok(l, px, py, vx, vy, np.stack([blk, blk]), nid, copper):
                    best = (round(vx, 3), round(vy, 3))
                    break
            if best:
                break
        big = (p["bbox"][2] - p["bbox"][0]) * (p["bbox"][3] - p["bbox"][1]) >= 1.0
        if best is None and big:
            best = (px, py)                   # exposed pad: via in the pad centre
        if best is None:
            fanout["skipped"].append([net, list(key)])
            continue
        vx, vy = best
        placed.append(best)
        fanout["tracks"].append([l, px, py, vx, vy, tw, net])
        fanout["vias"].append([vx, vy, VD, VDR, net])
        (j0, j1, i0, i1), m = raster_segment(px, py, vx, vy, tw / 2)
        pad_owner[l, j0:j1, i0:i1][m] = nid
        (j0, j1, i0, i1), m = raster_segment(vx, vy, vx, vy, VD / 2)
        for li in (0, 1):
            pad_owner[li, j0:j1, i0:i1][m] = nid
            pad_any[li, j0:j1, i0:i1] |= m
    print(f"plane fan-out: {len(fanout['vias'])} vias, skipped {fanout['skipped']}", flush=True)


def main():
    t0 = time.time()
    plane_fanout()
    key_of = {i: (c[0], c[1]) for i, c in enumerate(conns)}
    queue = list(range(len(conns)))
    attempts = {}
    failed = set()
    steps = 0
    while queue and steps < 4 * len(conns):
        steps += 1
        ci = queue.pop(0)
        net, key = key_of[ci]
        attempts[ci] = attempts.get(ci, 0) + 1
        res = route_conn(net, key, allow_soft=False)
        if res is None:
            res = route_conn(net, key, allow_soft=False, full_board=True)
        if res is None and attempts[ci] <= 4:
            res = route_conn(net, key, allow_soft=True, full_board=True)
            if res is not None:
                tracks, vias, cells = res
                vic_nets = {key_of[v][0] for v in victims_of(cells, net)} - {net}
                for s in cells:
                    l, rem = divmod(s, N); j, i = divmod(rem, W)
                    history[l, j, i] += 1.0
                # rip up every victim net completely so its tree stays consistent
                for vn in vic_nets:
                    for idx, (n2, k2) in key_of.items():
                        if n2 == vn and (n2, k2) in routes:
                            del routes[(n2, k2)]
                            if idx not in queue:
                                queue.append(idx)
                    connected[vn] = {seeds[vn]}
                rebuild()
                print(f"  rip-up for {net}: {sorted(vic_nets)}", flush=True)
        if res is None:
            failed.add(ci)
            print(f"  FAIL {net} -> {key}")
            continue
        failed.discard(ci)
        tracks, vias, _ = res
        routes[(net, key)] = {"net": net, "tracks": tracks, "vias": vias}
        paint((net, key), net_id[net], ci)
        connected[net].add(key)
        if len(routes) % 20 == 0:
            print(f"  {len(routes)}/{len(conns)} routed, queue {len(queue)}, {time.time() - t0:.0f}s", flush=True)
    out = {"tracks": list(fanout["tracks"]), "vias": list(fanout["vias"]), "failed": [],
           "fanout_skipped": fanout["skipped"]}
    for (net, key), r in routes.items():
        out["tracks"] += [[l, a, b, c, d, w, net] for l, a, b, c, d, w in r["tracks"]]
        out["vias"] += [[x, y, d, dr, net] for x, y, d, dr in r["vias"]]
    out["failed"] = [[key_of[c][0], list(key_of[c][1])] for c in failed if key_of[c] not in routes]
    json.dump(out, open("routes.json", "w"))
    print(f"done: {len(routes)}/{len(conns)} connections, {len(out['tracks'])} tracks, "
          f"{len(out['vias'])} vias, {len(out['failed'])} failed, {time.time() - t0:.0f}s")


if __name__ == "__main__":
    main()
