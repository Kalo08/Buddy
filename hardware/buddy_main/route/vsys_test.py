import sys, time
sys.argv = [sys.argv[0], "geom.json"]
import router as R
R.plane_fanout()
vs = [i for i, c in enumerate(R.conns) if c[0] == "VSYS"]
print("VSYS connections:", len(vs), "width", R.POWER["VSYS"])
t0 = time.time()
ok = 0
for ci in vs:
    net, key, _ = R.conns[ci][0], R.conns[ci][1], None
    res = R.route_conn(net, key, allow_soft=False, full_board=True)
    if res is None:
        print("  FAIL", key, f"{time.time()-t0:.0f}s")
        continue
    tracks, vias, _c = res
    R.routes[(net, key)] = {"net": net, "tracks": tracks, "vias": vias}
    R.paint((net, key), R.net_id[net], ci)
    R.connected[net].add(key)
    ok += 1
    print("  ok", key, len(tracks), "tracks", len(vias), "vias", f"{time.time()-t0:.0f}s", flush=True)
print("routed", ok, "of", len(vs))
