import json, math, sys
d = json.load(open(sys.argv[1] if len(sys.argv) > 1 else "routes.json"))
bad = []
for l, xa, ya, xb, yb, w, net in d["tracks"]:
    L = math.hypot(xb - xa, yb - ya)
    if L < 1e-6:
        continue
    a = math.degrees(math.atan2(yb - ya, xb - xa)) % 45
    off = min(a, 45 - a)
    if off > 0.05:
        bad.append((round(off, 2), round(L, 2), net))
print("tracks:", len(d["tracks"]), "off 45/90 by more than 0.05 deg:", len(bad), bad[:8])
