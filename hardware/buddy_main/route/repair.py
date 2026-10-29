"""DRC-driven repair: rip up every trace/via KiCad flags, re-route with more room.

Needs routes.json from router.py (with per-connection data). Each round:
  1. build routed.kicad_pcb from routes.json (KiCad's python) and run kicad-cli DRC
  2. map each flagged track / via back to its connection and rip it up
  3. re-route those connections with a larger safety margin
Writes routes.json in place (a copy of the original is kept as routes_before_repair.json).
"""
import json
import math
import shutil
import subprocess
import sys

ROUNDS = int(sys.argv[1]) if len(sys.argv) > 1 else 5
sys.argv = [sys.argv[0], "geom.json"]
import router as R

KPY = "C:/Users/Gavin/Downloads/bin/python.exe"
KCLI = "C:/Users/Gavin/Downloads/bin/kicad-cli.exe"
BAD = {"clearance", "shorting_items", "hole_clearance", "solder_mask_bridge", "copper_edge_clearance",
       "tracks_crossing", "via_dangling", "track_dangling"}
data = json.load(open("routes.json"))
shutil.copy("routes.json", "routes_before_repair.json")
key_of = {i: (c[0], c[1]) for i, c in enumerate(R.conns)}
idx_of = {v: k for k, v in key_of.items()}
for c in data["conns"]:
    R.routes[(c["net"], tuple(c["key"]))] = {"net": c["net"], "tracks": [tuple(t) for t in c["tracks"]],
                                               "vias": [tuple(v) for v in c["vias"]]}
R.plane_fanout()
R.FROZEN.update(R.net_id[n] for n in R.POWER if n in R.net_id and any(k[0] == n for k in R.routes))
failed = {idx_of[(n, tuple(k))] for n, k in data["failed"] if (n, tuple(k)) in idx_of}


def reset_state():
    for net, k in R.seeds.items():
        R.connected[net] = {k}
    for (net, key) in R.routes:
        R.connected[net].add(key)
    R.rebuild()


def save():
    out = {"tracks": list(R.fanout["tracks"]), "vias": list(R.fanout["vias"]), "failed": [],
           "fanout_skipped": R.fanout["skipped"], "conns": []}
    for (net, key), r in R.routes.items():
        out["tracks"] += [[l, a, b, c, d, w, net] for l, a, b, c, d, w in r["tracks"]]
        out["vias"] += [[x, y, d, dr, net] for x, y, d, dr in r["vias"]]
        out["conns"].append({"net": net, "key": list(key), "tracks": [list(t) for t in r["tracks"]],
                             "vias": [list(v) for v in r["vias"]]})
    out["failed"] = [[key_of[c][0], list(key_of[c][1])] for c in sorted(failed) if key_of[c] not in R.routes]
    json.dump(out, open("routes.json", "w"))


def run_drc():
    subprocess.run([KPY, "apply.py", "work.kicad_pcb", "routed.kicad_pcb", "buddy_main.ses"],
                   check=True, capture_output=True)
    subprocess.run([KCLI, "pcb", "drc", "--severity-error", "--format", "json", "--all-track-errors",
                    "-o", "drc_routed.json", "routed.kicad_pcb"], check=True, capture_output=True)
    return json.load(open("drc_routed.json"))["violations"]


def owner_of(desc, pos):
    """Connection key owning the flagged track/via, or None (pads belong to nobody)."""
    kind = desc.split(" ")[0]
    if kind not in ("Track", "Via"):
        return None
    net = desc[desc.index("[") + 1:desc.index("]")]
    x, y = pos["x"], pos["y"]
    for (n, key), r in R.routes.items():
        if n != net:
            continue
        if kind == "Via":
            if any(math.hypot(vx - x, vy - y) < 0.05 for vx, vy, d, dr in r["vias"]):
                return (n, key)
            continue
        for l, xa, ya, xb, yb, w in r["tracks"]:
            dx, dy = xb - xa, yb - ya
            L2 = dx * dx + dy * dy or 1e-12
            t = max(0.0, min(1.0, ((x - xa) * dx + (y - ya) * dy) / L2))
            if math.hypot(xa + t * dx - x, ya + t * dy - y) < 0.06:
                return (n, key)
    return None


def reroute(todo, margin):
    R.MARGIN = margin
    left = []
    for ci in sorted(todo, key=lambda c: (0 if key_of[c][0] in R.POWER else 1, c)):
        net, key = key_of[ci]
        if (net, key) in R.routes:
            continue
        res = R.route_conn(net, key, allow_soft=False) or R.route_conn(net, key, allow_soft=False, full_board=True)
        if res is None:
            left.append(ci)
            continue
        tracks, vias, _ = res
        R.routes[(net, key)] = {"net": net, "tracks": tracks, "vias": vias}
        R.paint((net, key), R.net_id[net], ci)
        R.connected[net].add(key)
    R.MARGIN = 0.10
    return left


def finish(unrouted):
    """Route stuck connections through foreign copper, then re-route whatever got displaced."""
    left = set()
    for ci in sorted(unrouted):
        net, key = key_of[ci]
        if (net, key) in R.routes:
            continue
        res = R.route_conn(net, key, allow_soft=True, full_board=True)
        if res is None:
            left.add(ci)
            continue
        tracks, vias, cells = res
        for s in cells:
            l, rem = divmod(s, R.N)
            j, i = divmod(rem, R.W)
            R.history[l, j, i] += 1.0
        victims = {key_of[v][0] for v in R.victims_of(cells, net)} - {net}
        redo = set()
        for vn in victims:
            for idx, (n2, k2) in key_of.items():
                if n2 == vn and (n2, k2) in R.routes:
                    del R.routes[(n2, k2)]
                    redo.add(idx)
        R.routes[(net, key)] = {"net": net, "tracks": tracks, "vias": vias}
        reset_state()
        R.paint((net, key), R.net_id[net], ci)
        left |= set(reroute(redo, 0.10))
        print(f"  finished {net} -> {key}; displaced {sorted(victims)}", flush=True)
    return left


reset_state()
failed = set(reroute(failed, 0.10))
reset_state()
for rnd in range(1, ROUNDS + 1):
    viol = [v for v in run_drc() if v["type"] in BAD]
    bad = set()
    for v in viol:
        owners = {owner_of(it["description"], it["pos"]) for it in v["items"]} - {None}
        movable = {o for o in owners if o[0] not in R.POWER}
        bad |= movable or owners          # a signal yields to a frozen supply net
    print(f"round {rnd}: {len(viol)} violations, {len(bad)} connections to redo, {len(failed)} unrouted", flush=True)
    if not viol and not failed:
        break
    if not bad and not failed:
        break
    todo = set(failed)
    # a dangling child would be left behind by ripping one connection: rip the whole net
    for net in {o[0] for o in bad}:
        for key in [k for (n, k) in R.routes if n == net]:
            del R.routes[(net, key)]
        todo |= {i for i, (n, k) in key_of.items() if n == net}
    reset_state()
    # more room each round; connections that still fail stay on the unrouted list
    failed = set(reroute(todo, 0.10 + 0.04 * rnd))
    reset_state()
    save()
viol = [v for v in run_drc() if v["type"] in BAD]
save()
print(f"final: {len(viol)} violations, {len(failed)} unrouted: {[key_of[c] for c in sorted(failed)]}")
