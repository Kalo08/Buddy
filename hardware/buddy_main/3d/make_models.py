"""Generate simple box-based STEP models for parts KiCad's 3D library lacks.

KiCad ships footprints for these parts but no 3D models (checked against
gitlab.com/kicad/libraries/kicad-packages3D, 2026-10-29). Each model is a set
of coloured axis-aligned boxes sized from the manufacturer's drawing, which is
enough for enclosure fit checks and renders.

Run: python make_models.py   (writes the .step files next to this script)
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# name -> list of (x0, y0, z0, x1, y1, z1, (r, g, b)), millimetres,
# footprint origin, +z up from the board surface.
YELLOW = (0.93, 0.75, 0.10)
GOLD = (0.85, 0.65, 0.20)
PTC = (0.80, 0.62, 0.25)
TIN = (0.80, 0.80, 0.82)
MODELS = {
    # AMASS XT30U-M, vertical PCB mount. Pins at (0,0) and (5,0).
    "AMASS_XT30U-M_1x02_P5.0mm_Vertical": [
        (-2.6, -2.6, 0.0, 7.6, 2.6, 8.0, YELLOW),
        (-0.8, -0.8, 8.0, 0.8, 0.8, 8.4, GOLD),
        (4.2, -0.8, 8.0, 5.8, 0.8, 8.4, GOLD),
    ],
    # 2920 (7451 metric) SMD PTC fuse.
    "Fuse_2920_7451Metric": [
        (-2.9, -2.55, 0.0, 2.9, 2.55, 1.6, PTC),
        (-3.7, -2.55, 0.0, -2.9, 2.55, 1.7, TIN),
        (2.9, -2.55, 0.0, 3.7, 2.55, 1.7, TIN),
    ],
}


def real(v):
    """STEP REAL: always carries a decimal point."""
    t = repr(float(v))
    return t if ("." in t or "e" in t.lower()) else t + "."


def tup(vs):
    return "(" + ",".join(real(v) for v in vs) + ")"


class Step:
    def __init__(self):
        self.lines, self.n = [], 0

    def add(self, text):
        self.n += 1
        self.lines.append("#%d=%s;" % (self.n, text))
        return "#%d" % self.n

    def point(self, p):
        return self.add("CARTESIAN_POINT(''," + tup(p) + ")")

    def direction(self, d):
        return self.add("DIRECTION(''," + tup(d) + ")")


def box(s, x0, y0, z0, x1, y1, z1):
    """Add one closed box B-rep; returns the MANIFOLD_SOLID_BREP id."""
    # corner index = x + 2*y + 4*z
    corners = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    vtx = [s.add("VERTEX_POINT(''," + s.point(c) + ")") for c in corners]
    edges = {}

    def edge(a, b):
        if (a, b) in edges:
            return edges[(a, b)], True
        if (b, a) in edges:
            return edges[(b, a)], False
        pa, pb = corners[a], corners[b]
        d = [pb[i] - pa[i] for i in range(3)]
        length = max(abs(c) for c in d)
        unit = [c / length for c in d]
        vec = s.add("VECTOR(''," + s.direction(unit) + "," + real(length) + ")")
        line = s.add("LINE(''," + s.point(pa) + "," + vec + ")")
        edges[(a, b)] = s.add("EDGE_CURVE(''," + vtx[a] + "," + vtx[b] + "," + line + ",.T.)")
        return edges[(a, b)], True

    # loops run counter-clockwise seen from outside the box
    faces = [
        ((0, 2, 3, 1), (0, 0, -1), (1, 0, 0), (x0, y0, z0)),   # bottom
        ((4, 5, 7, 6), (0, 0, 1), (1, 0, 0), (x0, y0, z1)),    # top
        ((0, 1, 5, 4), (0, -1, 0), (1, 0, 0), (x0, y0, z0)),   # -y side
        ((2, 6, 7, 3), (0, 1, 0), (1, 0, 0), (x0, y1, z0)),    # +y side
        ((0, 4, 6, 2), (-1, 0, 0), (0, 1, 0), (x0, y0, z0)),   # -x side
        ((1, 3, 7, 5), (1, 0, 0), (0, 1, 0), (x1, y0, z0)),    # +x side
    ]
    face_ids = []
    for loop, normal, ref, origin in faces:
        oriented = []
        for i in range(4):
            e, forward = edge(loop[i], loop[(i + 1) % 4])
            oriented.append(s.add("ORIENTED_EDGE('',*,*," + e + "," + (".T." if forward else ".F.") + ")"))
        loop_id = s.add("EDGE_LOOP('',(" + ",".join(oriented) + "))")
        bound = s.add("FACE_OUTER_BOUND(''," + loop_id + ",.T.)")
        axis = s.add("AXIS2_PLACEMENT_3D(''," + s.point(origin) + ","
                     + s.direction(normal) + "," + s.direction(ref) + ")")
        plane = s.add("PLANE(''," + axis + ")")
        face_ids.append(s.add("ADVANCED_FACE('',(" + bound + ")," + plane + ",.T.)"))
    shell = s.add("CLOSED_SHELL('',(" + ",".join(face_ids) + "))")
    return s.add("MANIFOLD_SOLID_BREP(''," + shell + ")")


def write_model(name, boxes):
    s = Step()
    length = s.add("(LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.))")
    angle = s.add("(NAMED_UNIT(*) PLANE_ANGLE_UNIT() SI_UNIT($,.RADIAN.))")
    solid_angle = s.add("(NAMED_UNIT(*) SI_UNIT($,.STERADIAN.) SOLID_ANGLE_UNIT())")
    unc = s.add("UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-07)," + length
                + ",'distance_accuracy_value','confusion accuracy')")
    ctx = s.add("(GEOMETRIC_REPRESENTATION_CONTEXT(3) GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((" + unc + ")) "
                "GLOBAL_UNIT_ASSIGNED_CONTEXT((" + length + "," + angle + "," + solid_angle + ")) "
                "REPRESENTATION_CONTEXT('Context3D','3D Context with 1E-07 mm uncertainty'))")
    origin = s.add("AXIS2_PLACEMENT_3D(''," + s.point((0, 0, 0)) + ","
                   + s.direction((0, 0, 1)) + "," + s.direction((1, 0, 0)) + ")")
    solids, styled = [], []
    for x0, y0, z0, x1, y1, z1, rgb in boxes:
        solid = box(s, x0, y0, z0, x1, y1, z1)
        solids.append(solid)
        colour = s.add("COLOUR_RGB(''," + ",".join(real(c) for c in rgb) + ")")
        fill_colour = s.add("FILL_AREA_STYLE_COLOUR(''," + colour + ")")
        fill = s.add("FILL_AREA_STYLE('',(" + fill_colour + "))")
        side = s.add("SURFACE_SIDE_STYLE('',(" + s.add("SURFACE_STYLE_FILL_AREA(" + fill + ")") + "))")
        usage = s.add("SURFACE_STYLE_USAGE(.BOTH.," + side + ")")
        style = s.add("PRESENTATION_STYLE_ASSIGNMENT((" + usage + "))")
        styled.append(s.add("STYLED_ITEM('color',(" + style + ")," + solid + ")"))
    rep = s.add("ADVANCED_BREP_SHAPE_REPRESENTATION('" + name + "',(" + ",".join(solids) + "," + origin + ")," + ctx + ")")
    s.add("MECHANICAL_DESIGN_GEOMETRIC_PRESENTATION_REPRESENTATION('',(" + ",".join(styled) + ")," + ctx + ")")
    app = s.add("APPLICATION_CONTEXT('core data for automotive mechanical design processes')")
    s.add("APPLICATION_PROTOCOL_DEFINITION('international standard','automotive_design',2000," + app + ")")
    pctx = s.add("PRODUCT_CONTEXT(''," + app + ",'mechanical')")
    prod = s.add("PRODUCT('" + name + "','" + name + "','',(" + pctx + "))")
    s.add("PRODUCT_RELATED_PRODUCT_CATEGORY('part',$,(" + prod + "))")
    pdf = s.add("PRODUCT_DEFINITION_FORMATION('',''," + prod + ")")
    dctx = s.add("PRODUCT_DEFINITION_CONTEXT('part definition'," + app + ",'design')")
    pd = s.add("PRODUCT_DEFINITION('design',''," + pdf + "," + dctx + ")")
    pds = s.add("PRODUCT_DEFINITION_SHAPE(''," + "''," + pd + ")")
    s.add("SHAPE_DEFINITION_REPRESENTATION(" + pds + "," + rep + ")")
    header = (
        "ISO-10303-21;\nHEADER;\n"
        "FILE_DESCRIPTION(('Simplified model of " + name + "'),'2;1');\n"
        "FILE_NAME('" + name + ".step','2026-10-29T00:00:00',('Buddy'),(''),'make_models.py','make_models.py','');\n"
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\nDATA;\n"
    )
    path = os.path.join(HERE, name + ".step")
    with open(path, "w", encoding="ascii") as f:
        f.write(header + "\n".join(s.lines) + "\nENDSEC;\nEND-ISO-10303-21;\n")
    return path


if __name__ == "__main__":
    for name, boxes in MODELS.items():
        print(write_model(name, boxes))
