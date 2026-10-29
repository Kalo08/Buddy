"""Build the 72 x 62 mm board copy: unrouted 4-layer board + new placement + new outline."""
import json
import math

import pcbnew

FM = pcbnew.FromMM
V = lambda x, y: pcbnew.VECTOR2I(FM(x), FM(y))
board = pcbnew.LoadBoard("../backup/buddy_main_4layer_unrouted.kicad_pcb")

for t in list(board.GetTracks()):
    board.Remove(t)
for z in list(board.Zones()):
    board.Remove(z)

place = {p["reference"]: p for p in json.load(open("placements72.json"))}
place.update({h: {"x": x, "y": y, "rotation": 0} for h, (x, y) in
              {"H1": (103.5, 103.5), "H2": (168.5, 103.5), "H3": (103.5, 158.5), "H4": (168.5, 158.5)}.items()})
moved = 0
for fp in board.GetFootprints():
    p = place.get(fp.GetReference())
    if p:
        fp.SetPosition(V(p["x"], p["y"]))
        fp.SetOrientationDegrees(p["rotation"])
        moved += 1

for d in list(board.GetDrawings()):
    if d.GetLayer() == pcbnew.Edge_Cuts:
        board.Remove(d)
x1, y1, x2, y2, r = 100.0, 100.0, 172.0, 162.0, 4.0


def seg(a, b):
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(V(*a)); s.SetEnd(V(*b)); s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(FM(0.05))
    board.Add(s)


def arc(c, a0):
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
    pt = lambda deg: (c[0] + r * math.cos(math.radians(deg)), c[1] + r * math.sin(math.radians(deg)))
    s.SetArcGeometry(V(*pt(a0)), V(*pt(a0 + 45)), V(*pt(a0 + 90)))
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(FM(0.05))
    board.Add(s)


seg((x1 + r, y1), (x2 - r, y1)); seg((x2, y1 + r), (x2, y2 - r))
seg((x2 - r, y2), (x1 + r, y2)); seg((x1, y2 - r), (x1, y1 + r))
arc((x1 + r, y1 + r), 180); arc((x2 - r, y1 + r), 270); arc((x2 - r, y2 - r), 0); arc((x1 + r, y2 - r), 90)
board.Save("work.kicad_pcb")
print("moved", moved, "footprints; outline", x1, y1, x2, y2)
