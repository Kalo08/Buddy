"""Try each unrouted connection one at a time; keep it only if DRC stays clean.

Starts from routes.json (the clean state), writes routes.json when done.
"""
import copy
import json
import math
import subprocess
import sys

sys.argv = [sys.argv[0], "geom.json"]
import router as R

KPY = "C:/Users/Gavin/Downloads/bin/python.exe"
KCLI = "C:/Users/Gavin/Downloads/bin/kicad-cli.exe"
data = json.load(open("routes.json"))
key_of = {i: (c[0], c[1]) for i, c in enumerate(R.conns)}
idx_of = {v: k for k, v in key_of.items()}
for c in data["conns"]:
    R.routes[(c["net"], tuple(c["key"]))] = {"net": c["net"], "tracks": [tuple(t) for t in c["tracks"]],
                                               "vias": [tuple(v) for v in c["vias"]]}
R.plane_fanout()
R.FROZEN.update(R.net_id[n] for n in R.POWER if n in R.net_id and any(k[0] == n for k in R.routes))


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
    out["failed"] = [[n, list(k)] for (n, k) in (key_of[c] for c in range(len(R.conns))) if (n, k) not in R.routes]
    json.dump(out, open("routes.json", "w"))


def drc_count():
    save()
    subprocess.run([KPY, "apply.py", "work.kicad_pcb", "routed.kicad_pcb", "buddy_main.ses"], check=True, capture_output=True)
    subprocess.run([KCLI, "pcb", "drc", "--severity-error", "--format", "json", "--all-track-errors",
                    "-o", "drc_try.json", "routed.kicad_pcb"], check=True, capture_output=True)
    d = json.load(open("drc_try.json"))
    unconn = [u for u in d["unconnected_items"]
              if not all("GND_" in i["description"] for i in u["items"])]
    return len(d["violations"]), len(unconn)


def route_one(ci):
    net, key = key_of[ci]
    res = R.route_conn(net, key, allow_soft=False, full_board=True)
    if res is None:
        res = R.route_conn(net, key, allow_soft=True, full_board=True)
    if res is None:
        return None
    tracks, vias, cells = res
    victims = {key_of[v][0] for v in R.victims_of(cells, net)} - {net}
    return tracks, vias, cells, victims


base_v, base_u = drc_count()
print(f"baseline: {base_v} violations, {base_u} open items, {len(R.conns) - len(R.routes)} unrouted", flush=True)
todo = [ci for ci in range(len(R.conns)) if key_of[ci] not in R.routes]
todo.sort(key=lambda c: (0 if key_of[c][0] in R.POWER else 1, c))
kept, dropped = [], []
for ci in todo:
    net, key = key_of[ci]
    snap = copy.deepcopy(R.routes)
    reset_state()
    got = route_one(ci)
    if got is None:
        dropped.append((net, key, "no path"))
        print(f"  skip {net} -> {key}: no path", flush=True)
        continue
    tracks, vias, cells, victims = got
    redo = []
    for vn in victims:
        for idx, (n2, k2) in key_of.items():
            if n2 == vn and (n2, k2) in R.routes:
                del R.routes[(n2, k2)]
                redo.append(idx)
    R.routes[(net, key)] = {"net": net, "tracks": tracks, "vias": vias}
    reset_state()
    R.paint((net, key), R.net_id[net], ci)
    ok = True
    for rc in sorted(redo):
        n2, k2 = key_of[rc]
        res = R.route_conn(n2, k2, allow_soft=False) or R.route_conn(n2, k2, allow_soft=False, full_board=True)
        if res is None:
            ok = False
            break
        t2, v2, _ = res
        R.routes[(n2, k2)] = {"net": n2, "tracks": t2, "vias": v2}
        R.paint((n2, k2), R.net_id[n2], rc)
        R.connected[n2].add(k2)
    if ok:
        R.connected[net].add(key)
        v, u = drc_count()
        ok = v <= base_v and u <= base_u
    if ok:
        kept.append((net, key, sorted(victims)))
        print(f"  KEPT {net} -> {key} (moved {sorted(victims)})", flush=True)
    else:
        R.routes.clear()
        R.routes.update(snap)
        dropped.append((net, key, "would break DRC"))
        print(f"  undo {net} -> {key}", flush=True)
    reset_state()
reset_state()
v, u = drc_count()
print(f"final: {v} violations, {u} open items; kept {len(kept)}, left open {len(dropped)}")
print("open:", [(n, k) for n, k, _ in dropped])
