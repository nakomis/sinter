"""Write a Specctra DSN (the input to Freerouting) from a Circuit and a Placement.

Units: 'um' with resolution 1, so 1 mm = 1000 DSN units. (Freerouting 2.5.0 writes its SES
10x larger than the stated resolution when the DSN says 'resolution um 10', so we use 1.) DSN is y-up, KiCad y-down, hence
every y is negated. Pad coordinates stay in the footprint frame; the placement rotation goes in
(place ... rot), which is counter-clockwise in both frames.
"""
import re
from pathlib import Path

from board.netlist import Circuit  # noqa: F401  (documentation of the argument type)

SCALE = 1000                        # DSN units per mm
POWER = ("+5V", "+3V3", "+1V5", "+1V1", "VTT", "GND")
# Freerouting is told the clearances are this much larger than the design rules, because it
# narrows some pad-escape stubs (which read_ses() then widens) and rounds pad outlines.
ROUTER_MARGIN = 0.01
_PLAIN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def _q(name):
    return name if _PLAIN.match(name) else f'"{name}"'


def _u(v):
    return f"{round(v * SCALE)}"


def _layers(layers):
    return ["F.Cu", "B.Cu"] if layers == 2 else ["F.Cu", "In1.Cu", "In2.Cu", "B.Cu"]


def _padstack(pad, copper):
    """(name, text) for one pad. Pad rotation is folded into the shape (90/270 swaps w and h)."""
    w, h = (pad.h, pad.w) if round(pad.rot) % 180 == 90 else (pad.w, pad.h)
    tht = pad.kind == "thru_hole"
    on = copper if tht else ["F.Cu"]
    shape = pad.shape
    if shape == "circle":
        d = min(w, h)
        shapes = [f"(shape (circle {l} {_u(d)}))" for l in on]
        key = f"c{_u(d)}"
    elif shape == "oval" and abs(w - h) > 1e-6:
        d, half = min(w, h), (abs(w - h) / 2)
        pts = f"0 {_u(-half)} 0 {_u(half)}" if h > w else f"{_u(-half)} 0 {_u(half)} 0"
        shapes = [f"(shape (path {l} {_u(d)} {pts}))" for l in on]
        key = f"o{_u(w)}x{_u(h)}"
    elif shape == "oval":
        shapes = [f"(shape (circle {l} {_u(w)}))" for l in on]
        key = f"c{_u(w)}"
    else:
        rect = f"{_u(-w / 2)} {_u(-h / 2)} {_u(w / 2)} {_u(h / 2)}"
        shapes = [f"(shape (rect {l} {rect}))" for l in on]
        key = f"r{_u(w)}x{_u(h)}"
    name = f"{'T' if tht else 'S'}_{key}"
    return name, f"(padstack {_q(name)} {' '.join(shapes)} (attach off))"


def write_dsn(circuit, placement, path, layers=4):
    if layers not in (2, 4):
        raise ValueError("layers must be 2 or 4")
    copper = _layers(layers)
    x0, y0, x1, y1 = placement.outline
    corner = lambda x, y: f"{_u(x)} {_u(-y)}"
    outline = " ".join(corner(*p) for p in ((x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)))
    via = f"Via[0-{layers - 1}]_600:300_um"
    out = ["(pcb channel", '  (parser (string_quote ") (space_in_quoted_tokens on) (host_cad "sint101"))',
           "  (resolution um 1)", "  (unit um)", "  (structure"]
    for l in copper:
        out.append(f"    (layer {l} (type signal))")
    out.append(f"    (boundary (path pcb 0 {outline}))")
    out.append(f"    (via {_q(via)})")
    out.append(f"    (rule (width {_u(0.15)}) (clearance {_u(0.15)}))")
    if layers == 4:
        for net, layer in (("GND", "In1.Cu"), ("+1V5", "In2.Cu")):
            if net in circuit.nets:
                out.append(f"    (plane {_q(net)} (polygon {layer} 0 {outline}))")
    out.append("  )")

    stacks, images, comps = {}, [], []
    for ref, inst in circuit.instances.items():
        pins = []
        for pad in inst.part.footprint.pads:
            name, text = _padstack(pad, copper)
            stacks[name] = text
            pins.append(f"(pin {_q(name)} {_q(pad.number)} {_u(pad.x)} {_u(-pad.y)})")
        img = f"{ref}_img"
        images.append(f"    (image {img} {' '.join(pins)})")
        x, y, rot = placement.at[ref]
        comps.append(f"    (component {img} (place {_q(ref)} {_u(x)} {_u(-y)} front {rot:g}))")
    out.append("  (placement")
    out += comps
    out.append("  )")
    out.append("  (library")
    out += images
    out += [f"    {s}" for s in stacks.values()]
    vias = " ".join(f"(shape (circle {l} {_u(0.6)}))" for l in ("F.Cu", "B.Cu"))
    if layers == 4:
        vias = " ".join(f"(shape (circle {l} {_u(0.6)}))" for l in copper)
    out.append(f"    (padstack {_q(via)} {vias} (attach off))")
    out.append("  )")
    out.append("  (network")
    for name in sorted(circuit.nets):
        pins = " ".join(f"{r}-{p}" for r, p in circuit.nets[name])
        out.append(f"    (net {_q(name)} (pins {pins}))")
    # On a 2-layer board GND is carried by the B.Cu pour; the tracks are only connections to it,
    # and 0.5 mm tracks cannot be routed between 0.5 mm-pitch pads (Freerouting left 20 GND
    # connections open), so GND gets its own class there: default width, power clearance.
    # It must keep the Power class clearance (0.2 mm) from .kicad_pro; the router is given 0.25 mm
    # so that the narrowed stubs read_ses() widens back to 0.15 mm still clear.
    thin_gnd = layers == 2 and "GND" in circuit.nets
    power = [n for n in sorted(circuit.nets) if n in POWER and not (thin_gnd and n == "GND")]
    other = [n for n in sorted(circuit.nets) if n not in POWER]
    out.append(f"    (class default {' '.join(_q(n) for n in other)} "
               f"(circuit (use_via {_q(via)})) (rule (width {_u(0.15)}) (clearance {_u(ROUTER_MARGIN + 0.15)})))")
    if power:
        out.append(f"    (class power {' '.join(_q(n) for n in power)} "
                   f"(circuit (use_via {_q(via)})) (rule (width {_u(0.5)}) (clearance {_u(0.2)})))")
    if thin_gnd:
        out.append(f"    (class gnd GND (circuit (use_via {_q(via)})) "
                   f"(rule (width {_u(0.15)}) (clearance {_u(ROUTER_MARGIN + 0.2)})))")
    out.append("  )")
    out.append("  (wiring)")
    out.append(")")
    Path(path).write_text("\n".join(out) + "\n")
