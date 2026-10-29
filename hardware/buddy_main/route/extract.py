"""Dump pad / hole / outline geometry of a board to JSON for the router.

Run with KiCad's python:  python.exe extract.py <board.kicad_pcb> <out.json>
"""
import json
import sys

import pcbnew

board = pcbnew.LoadBoard(sys.argv[1])
mm = pcbnew.ToMM

pads = []
for fp in board.GetFootprints():
    for pad in fp.Pads():
        layers = [name for name, lid in (("F", pcbnew.F_Cu), ("B", pcbnew.B_Cu)) if pad.IsOnLayer(lid)]
        bb = pad.GetBoundingBox()
        drill = pad.GetDrillSize()
        pads.append({
            "ref": fp.GetReference(),
            "num": pad.GetNumber(),
            "net": pad.GetNetname(),
            "layers": layers,
            "x": mm(pad.GetPosition().x), "y": mm(pad.GetPosition().y),
            "bbox": [mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom())],
            "circle": pad.GetShape(pcbnew.F_Cu) == pcbnew.PAD_SHAPE_CIRCLE,
            "drill": max(mm(drill.x), mm(drill.y)),
            "npth": pad.GetAttribute() == pcbnew.PAD_ATTRIB_NPTH,
        })

edge = board.GetBoardEdgesBoundingBox()
out = {
    "pads": pads,
    "outline": [mm(edge.GetLeft()), mm(edge.GetTop()), mm(edge.GetRight()), mm(edge.GetBottom())],
    "nets": sorted({p["net"] for p in pads if p["net"]}),
}
json.dump(out, open(sys.argv[2], "w"), indent=0)
print(len(pads), "pads,", len(out["nets"]), "nets, outline", out["outline"])
