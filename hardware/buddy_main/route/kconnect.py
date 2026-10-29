import sys
sys.path.insert(0, r"C:\Users\Gavin\Downloads\kipy-site")
from kipy import KiCad
k = KiCad()
print("kicad", k.get_version())
b = k.get_board()
print("board:", b.name)
print("tracks", len(b.get_tracks()), "vias", len(b.get_vias()), "footprints", len(b.get_footprints()))
