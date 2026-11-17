import sys, math
sys.path.insert(0, r"C:\Users\Gavin\Downloads\kipy-site")
from kipy import KiCad
from kipy.board_types import BoardLayer
from kipy.util.units import to_mm
b = KiCad(timeout_ms=60000).get_board()
tracks = b.get_tracks()
def show(net, x, y, r):
    print(f"-- {net} near ({x},{y}) r={r}")
    for t in tracks:
        if t.net.name != net:
            continue
        sx, sy, ex, ey = to_mm(t.start.x), to_mm(t.start.y), to_mm(t.end.x), to_mm(t.end.y)
        d = min(math.hypot(sx - x, sy - y), math.hypot(ex - x, ey - y))
        if d <= r:
            print("   %s (%.3f,%.3f)->(%.3f,%.3f) w%.2f  end-dist %.2f" % ("F" if t.layer == BoardLayer.BL_F_Cu else "B", sx, sy, ex, ey, to_mm(t.width), d))
for net, x, y in [("I2S_DIN", 151.6, 159.4), ("I2S_WS", 154.1, 159.4), ("/MCU and USB/UART_TX", 144.2, 129.1), ("/Audio and Camera/SPK_N", 168.4, 143.5)]:
    show(net, x, y, 4)
