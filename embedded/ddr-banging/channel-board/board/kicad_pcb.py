"""Write a KiCad 10 .kicad_pcb (and its .kicad_pro) from a Circuit and a Placement."""
import copy
import json
import uuid
from dataclasses import dataclass
from pathlib import Path

from board.placement import rotate
from board.sexpr import Sym, dumps, find, find_all

S = Sym
INSET = 0.5          # plane zones stop this far inside the outline
_NS = uuid.UUID("5a1d101c-0000-4000-8000-000000000101")

_USER_LAYERS = [
    [32, "B.Adhes", "user", "B.Adhesive"], [33, "F.Adhes", "user", "F.Adhesive"],
    [34, "B.Paste", "user"], [35, "F.Paste", "user"],
    [36, "B.SilkS", "user", "B.Silkscreen"], [37, "F.SilkS", "user", "F.Silkscreen"],
    [38, "B.Mask", "user"], [39, "F.Mask", "user"],
    [40, "Dwgs.User", "user", "User.Drawings"], [41, "Cmts.User", "user", "User.Comments"],
    [44, "Edge.Cuts", "user"], [45, "Margin", "user"],
    [46, "B.CrtYd", "user", "B.Courtyard"], [47, "F.CrtYd", "user", "F.Courtyard"],
    [48, "B.Fab", "user"], [49, "F.Fab", "user"],
]


@dataclass
class Track:
    net: str
    layer: str
    x1: float
    y1: float
    x2: float
    y2: float
    width: float


@dataclass
class Via:
    net: str
    x: float
    y: float


class _Uuids:
    """Deterministic uuids, so regenerating the board gives a byte-identical file."""
    def __init__(self):
        self.n = 0

    def __call__(self):
        self.n += 1
        return [S("uuid"), str(uuid.uuid5(_NS, str(self.n)))]


def pin_marks(circuit, placement, marks, inset=2.4):
    """Silkscreen entries (text, x, y, size) for (ref, pad, text, toward) tuples: the text sits
    'inset' mm from that pad's centre, towards the board point 'toward' (an (x, y) pair), clear
    of the part's own outline. Rotation of the part is applied to the pad position."""
    out = []
    for ref, number, text, toward in marks:
        x, y, rot = placement.at[ref]
        pad = next(p for p in circuit.instances[ref].part.footprint.pads if p.number == number)
        dx, dy = rotate(pad.x, pad.y, rot)
        px, py = x + dx, y + dy
        tx, ty = toward
        d = max(((tx - px) ** 2 + (ty - py) ** 2) ** 0.5, 1e-9)
        out.append((text, px + inset * (tx - px) / d, py + inset * (ty - py) / d, 1.0))
    return out


def _modern_arc(arc):
    """Legacy fp_arc (start = centre, end = first point, angle) -> start/mid/end.
    Legacy angles are clockwise on screen, hence the sign flip against placement.rotate."""
    centre, first = find(arc, "start"), find(arc, "end")
    angle = find(arc, "angle")[1]
    rx, ry = first[1] - centre[1], first[2] - centre[2]
    pt = lambda a: [centre[1] + rotate(rx, ry, a)[0], centre[2] + rotate(rx, ry, a)[1]]
    arc[:] = [v for v in arc if not (isinstance(v, list) and v[0] in ("start", "end", "angle"))]
    arc[1:1] = [[S("start"), first[1], first[2]], [S("mid")] + pt(-angle / 2), [S("end")] + pt(-angle)]


def _footprint(inst, x, y, rot, nets, uid, hide_ref=False):
    fp = copy.deepcopy(inst.part.footprint.node)
    fp[0] = S("footprint")
    fp[1] = str(fp[1]).split(":")[-1]      # the library nickname is not configured in the project
    fp[:] = [v for v in fp if not (isinstance(v, list) and v and v[0] in ("model", "tedit", "at", "uuid"))]
    i = next(k for k, v in enumerate(fp) if isinstance(v, list) and v and v[0] == "layer")
    fp[i + 1:i + 1] = [[S("at"), x, y, rot] if rot else [S("at"), x, y], uid()]
    attr = find(fp, "attr")
    if attr is None:
        attr = [S("attr"), S("through_hole" if inst.part.tht else "smd")]
        fp.insert(i + 3, attr)
    if inst.dnp:
        attr.append(S("dnp"))
    for t in find_all(fp, "fp_text"):
        if t[1] == "reference":
            t[2] = inst.ref
            if hide_ref:
                t.insert(t.index(find(t, "layer")) + 1, S("hide"))
        elif t[1] == "value":
            t[2] = inst.value
        if rot:                                      # text angles are absolute on a board
            at = find(t, "at")
            at[:] = at[:3] + [(at[3] if len(at) > 3 else 0) + rot]
    for arc in find_all(fp, "fp_arc"):
        if find(arc, "angle") is not None:
            _modern_arc(arc)
    for pad in find_all(fp, "pad"):
        if rot:                                      # pad angles are absolute on a board
            at = find(pad, "at")
            at[:] = at[:3] + [(at[3] if len(at) > 3 else 0) + rot]
        net = nets.get(str(pad[1]))
        if net is not None:
            pad.append([S("net"), net[0], net[1]])
        pad.append(uid())
    return fp


def _zone(net_id, name, layer, outline, uid):
    x0, y0, x1, y1 = outline
    x0, y0, x1, y1 = x0 + INSET, y0 + INSET, x1 - INSET, y1 - INSET
    return [S("zone"), [S("net"), net_id], [S("net_name"), name], [S("layer"), layer], uid(),
            [S("hatch"), S("edge"), 0.5],
            [S("connect_pads"), [S("clearance"), 0.2]],
            [S("min_thickness"), 0.2],
            [S("fill"), S("yes"), [S("thermal_gap"), 0.3], [S("thermal_bridge_width"), 0.4]],
            [S("polygon"), [S("pts"), [S("xy"), x0, y0], [S("xy"), x1, y0],
                            [S("xy"), x1, y1], [S("xy"), x0, y1]]]]


def _text(text, x, y, size, uid):
    return [S("gr_text"), text, [S("at"), x, y], [S("layer"), "F.SilkS"], uid(),
            [S("effects"), [S("font"), [S("size"), size, size], [S("thickness"), round(size * 0.15, 3)]]]]


PRO = {
    "meta": {"filename": "channel.kicad_pro", "version": 1},
    "board": {"design_settings": {"rules": {
        "min_clearance": 0.15, "min_track_width": 0.15, "min_via_diameter": 0.5,
        "min_through_hole_diameter": 0.3, "min_copper_edge_clearance": 0.3}}},
    "net_settings": {
        "meta": {"version": 3},      # without this KiCad ignores the classes and uses 0.2 mm
        "classes": [
            {"name": "Default", "clearance": 0.15, "track_width": 0.15, "via_diameter": 0.6, "via_drill": 0.3},
            {"name": "Power", "clearance": 0.2, "track_width": 0.5, "via_diameter": 0.6, "via_drill": 0.3}],
        "netclass_patterns": [{"netclass": "Power", "pattern": p} for p in ("+*", "GND", "VTT")]},
}


def write_board(circuit, placement, path, tracks=(), vias=(), layers=4, silk=(), hide_refs=(), art=None):
    """Write 'path' (.kicad_pcb) and a .kicad_pro beside it.

    layers=4: F.Cu, In1.Cu (GND zone), In2.Cu (+1V5 zone, only if that net exists), B.Cu.
    layers=2: F.Cu and B.Cu, with a GND zone on B.Cu. silk: (text, x, y, size_mm) entries.
    hide_refs: refs whose reference text is not drawn.
    art: (png_path, x, y, height_mm): a silk portrait, top-left at (x, y); see board/art.py."""
    if layers not in (2, 4):
        raise ValueError("layers must be 2 or 4")
    path = Path(path)
    uid = _Uuids()
    names = sorted(circuit.nets)
    ids = {n: i + 1 for i, n in enumerate(names)}
    cu = [[0, "F.Cu", "signal"]]
    if layers == 4:
        cu += [[1, "In1.Cu", "power"], [2, "In2.Cu", "power"]]
    cu.append([31, "B.Cu", "signal"])
    tree = [S("kicad_pcb"), [S("version"), 20240108], [S("generator"), "sint101_channel_board"],
            [S("generator_version"), "8.0"],
            [S("general"), [S("thickness"), 1.6]],
            [S("paper"), "A3"],
            [S("layers")] + [[n, name, S(kind), *alias] for n, name, kind, *alias in cu + _USER_LAYERS],
            [S("setup"), [S("pad_to_mask_clearance"), 0]],
            [S("net"), 0, ""]] + [[S("net"), ids[n], n] for n in names]
    x0, y0, x1, y1 = placement.outline
    tree.append([S("gr_rect"), [S("start"), x0, y0], [S("end"), x1, y1],
                 [S("stroke"), [S("width"), 0.1], [S("type"), S("default")]], [S("fill"), S("none")],
                 [S("layer"), "Edge.Cuts"], uid()])
    for ref, inst in circuit.instances.items():
        x, y, rot = placement.at[ref]
        nets = {pad: (ids[n], n) for n in names for (r, pad) in circuit.nets[n] if r == ref}
        tree.append(_footprint(inst, x, y, rot, nets, uid, ref in hide_refs))
    plane = [("GND", "In1.Cu" if layers == 4 else "B.Cu")]
    if layers == 4:
        plane.append(("+1V5", "In2.Cu"))
    for net, layer in plane:
        if net in ids:
            tree.append(_zone(ids[net], net, layer, placement.outline, uid))
    for t in tracks:
        tree.append([S("segment"), [S("start"), t.x1, t.y1], [S("end"), t.x2, t.y2], [S("width"), t.width],
                     [S("layer"), t.layer], [S("net"), ids[t.net]], uid()])
    for v in vias:
        tree.append([S("via"), [S("at"), v.x, v.y], [S("size"), 0.6], [S("drill"), 0.3],
                     [S("layers"), "F.Cu", "B.Cu"], [S("net"), ids[v.net]], uid()])
    if art:
        from board.art import art_footprint
        tree.append(art_footprint(*art))
    for text, x, y, size in silk:
        tree.append(_text(text, x, y, size, uid))
    path.write_text(dumps(tree).replace(") (", ")\n (") + "\n")
    path.with_suffix(".kicad_pro").write_text(json.dumps(PRO, indent=2) + "\n")
