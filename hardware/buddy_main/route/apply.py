"""Write routes.json + gnd.json into a board copy, pour GND on both layers,
and emit a Specctra session (.ses) for importing into the live board.

Run with KiCad's python:  python.exe apply.py <in.kicad_pcb> <out.kicad_pcb> <out.ses>
"""
import json
import sys

import pcbnew

src, dst, ses_path = sys.argv[1:4]
board = pcbnew.LoadBoard(src)
FM = pcbnew.FromMM
routes = json.load(open("routes.json"))
tracks = routes["tracks"]
vias = routes["vias"]
nets = board.GetNetsByName()

for l, xa, ya, xb, yb, w, net in tracks:
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I(FM(xa), FM(ya)))
    t.SetEnd(pcbnew.VECTOR2I(FM(xb), FM(yb)))
    t.SetWidth(FM(w))
    t.SetLayer(pcbnew.B_Cu if l else pcbnew.F_Cu)
    t.SetNet(nets[net])
    board.Add(t)
for x, y, d, dr, net in vias:
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I(FM(x), FM(y)))
    v.SetWidth(FM(d))
    v.SetDrill(FM(dr))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    v.SetNet(nets[net])
    board.Add(v)

# planes: In1 = GND, In2 = 3V3; outer layers get GND fill (KiCad clips to Edge.Cuts)
for layer, net, name, prio in ((pcbnew.In1_Cu, "GND", "GND_PLANE", 0), (pcbnew.In2_Cu, "3V3", "3V3_PLANE", 0),
                               (pcbnew.F_Cu, "GND", "GND_TOP", 0), (pcbnew.B_Cu, "GND", "GND_BOTTOM", 0)):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(nets[net])
    z.SetZoneName(name)
    z.SetAssignedPriority(prio)
    ol = z.Outline()
    ol.NewOutline()
    for x, y in ((99.5, 99.5), (164.5, 99.5), (164.5, 156.5), (99.5, 156.5)):
        ol.Append(FM(x), FM(y))
    z.SetMinThickness(FM(0.2))
    z.SetLocalClearance(FM(0.2))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(FM(0.25))
    z.SetThermalReliefSpokeWidth(FM(0.3))
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    board.Add(z)
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
board.Save(dst)

# Specctra session: DSN coordinates are micrometres with y negated
U = lambda v: int(round(v * 1000))
by_net = {}
for l, xa, ya, xb, yb, w, net in tracks:
    by_net.setdefault(net, [[], []])[0].append(
        f'      (wire (path {"B.Cu" if l else "F.Cu"} {U(w)} {U(xa)} {-U(ya)} {U(xb)} {-U(yb)}))')
for x, y, d, dr, net in vias:
    name = f'"Via[0-1]_{U(d)}:{U(dr)}_um"'
    by_net.setdefault(net, [[], []])[1].append(f"      (via {name} {U(x)} {-U(y)})")
lines = ['(session "buddy_main.ses"', '  (base_design "buddy_main.dsn")', '  (routes',
         '    (resolution um 1)', '    (parser (host_cad "KiCad\'s Pcbnew") (host_version "10.0.3"))',
         '    (network_out']
for net, (ws, vs) in by_net.items():
    lines.append(f'    (net "{net}"')
    lines += ws + vs
    lines.append('    )')
lines += ['    )', '  )', ')']
open(ses_path, "w").write("\n".join(lines) + "\n")
print(f"wrote {dst}: {len(tracks)} tracks, {len(vias)} vias; session {ses_path}")
