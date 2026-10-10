#!/usr/bin/env python3
"""Generate the two pogo-pin breakout PCBs for the DIMM fixture (SINT-100).

Each board bolts to the outer face of one jaw. The P75 pogo-pin barrels pass
through plated holes and are soldered; tracks take every pin to a labelled
2.54 mm header. Geometry comes from ../dimm-pogo-fixture.scad (pin lists,
pad positions, bolt positions are parsed from it), so the jaw and the PCB
cannot disagree.

Routing is a fixed two-layer channel scheme, so it needs no autorouter:
  * every terminal (pogo pin, header pin, divider pad) drops a vertical
    track on B.Cu to its net's own horizontal "lane";
  * lanes run horizontally on F.Cu, one per net, below the bolt holes;
  * vias join them.
Pins sit in three clusters along X; header pins and the VREF divider sit in
the two gaps between clusters, so no two vertical tracks share an X.

Coordinates: X along the DIMM from its pin-1 end, Z up from the DIMM's
bottom edge (as in the .scad). KiCad: y = top - z; the back board is
mirrored in X because it is seen from the other side.

    python3 gen_pcb.py            # writes front.kicad_pcb, back.kicad_pcb
"""
import re
import uuid
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
SCAD = (HERE.parent / "dimm-pogo-fixture.scad").read_text()


def scad_num(name):
    m = re.search(rf"^\s*{name}\s*=\s*([-0-9.]+)\s*;", SCAD, re.M)
    return float(m.group(1))


def scad_list(name):
    m = re.search(rf"{name}\s*=\s*\[(.*?)\];", SCAD, re.S)
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [float(v) if "." in v else int(v) for v in re.findall(r"[-0-9.]+", body)]


PIN1_X = scad_num("pin1_x")
PITCH = scad_num("pitch")
NOTCH_GAP = scad_num("notch_gap")
BOARD_LEN = scad_num("board_len")
END_CLR = scad_num("end_clr")
STOP_W = scad_num("stop_w")
ROW_Z = scad_list("row_z")
BOLT_X = scad_list("bolt_x")
FLOOR_H = scad_num("floor_h")
PAD_TOP = scad_num("pad_top")
FRONT_NEEDED = scad_list("front_needed")
BACK_NEEDED = scad_list("back_needed")
X0 = -(END_CLR + STOP_W + 0.5)          # matches x0 in the .scad
BOLT_Z = -FLOOR_H / 2


def pad_x(n):
    i = n - 120 if n > 120 else n
    return PIN1_X + (i - 1) * PITCH + (NOTCH_GAP if i >= 49 else 0)


def pad_z(n):
    i = n - 120 if n > 120 else n
    return ROW_Z[(i - 1) % 2]


# --- Netlist: DDR3 UDIMM pin -> net (JEDEC; Micron MT16JTF table 5) --------
# Pins tied off on the board go straight to VDD/VSS.
PIN_NET = {
    # front
    1: "VREF", 2: "VSS", 3: "DQ0", 4: "DQ1", 5: "VSS", 6: "DQS0#", 7: "DQS0", 8: "VSS",
    9: "DQ2", 10: "DQ3", 11: "VSS", 50: "CKE0", 51: "VDD", 52: "BA2", 54: "VDD",
    55: "A11", 56: "A7", 57: "VDD", 58: "A5", 59: "A4", 60: "VDD", 61: "A2", 62: "VDD",
    65: "VDD", 66: "VDD", 67: "VREF", 70: "A10", 71: "BA0", 73: "WE#", 74: "CAS#",
    76: "VDD",   # S1#  - rank 1 deselected
    77: "VSS",   # ODT1 - off
    80: "VSS", 83: "VSS",
    117: "VSS",  # SA0
    118: "SCL",
    119: "VSS",  # SA2
    120: "VTT",
    # back
    121: "VSS", 122: "DQ4", 123: "DQ5", 124: "VSS", 125: "DM0", 127: "VSS", 128: "DQ6",
    129: "DQ7", 130: "VSS", 168: "RESET#",
    169: "VSS",  # CKE1 - rank 1 held in power-down
    170: "VDD", 173: "VDD", 174: "A12", 175: "A9", 176: "VDD", 177: "A8", 178: "A6",
    179: "VDD", 180: "A3", 181: "A1", 182: "VDD", 183: "VDD", 184: "CK0", 185: "CK0#",
    186: "VDD", 188: "A0", 190: "BA1", 192: "RAS#", 193: "S0#", 195: "ODT0", 196: "A13",
    199: "VSS", 202: "VSS", 236: "VDDSPD",
    237: "VSS",  # SA1
    238: "SDA", 240: "VTT",
}

# Header order per board: gap A (between pin clusters 1-11 and 48-83), then gap B.
HEADERS = {
    "front": (["VSS", "DQ0", "DQ1", "DQ2", "DQ3", "DQS0", "DQS0#", "SCL", "VTT", "VREF", "VDD", "CKE0"],
              ["VSS", "BA0", "BA2", "A2", "A4", "A5", "A7", "A10", "A11", "WE#", "CAS#"]),
    "back": (["VSS", "DQ4", "DQ5", "DQ6", "DQ7", "DM0", "CK0", "CK0#", "SDA", "VDDSPD", "VTT", "VDD", "RESET#"],
             ["S0#", "ODT0", "RAS#", "BA1", "A0", "A1", "A3", "A6", "A8", "A9", "A12", "A13"]),
}

# --- Physical rules (JLCPCB standard 2-layer) ------------------------------
POGO_DRILL, POGO_PAD = 1.15, 1.41       # P75 barrel 1.02 mm; JLC PTH tolerance +0.13/-0.08
HDR_DRILL, HDR_PAD, HDR_PITCH = 1.0, 1.7, 2.54
RES_DRILL, RES_PAD, RES_SPAN = 0.8, 1.6, 7.62   # axial resistor, 0.3" pitch
CAP_SPAN = 5.08
VIA_D, VIA_DRILL = 0.6, 0.3
W_SIG, W_PWR, W_PIN_ZONE = 0.25, 0.6, 0.18
CLEAR = 0.2
POWER = {"VDD", "VSS"}
BOLT_HOLE = 3.2

GAP_A = (pad_x(11) + 2.5, pad_x(48) - 2.5)
GAP_B = (pad_x(83) + 2.5, pad_x(116) - 2.5)
Z_TOP = 6.0
LANE_TOP = BOLT_Z - BOLT_HOLE / 2 - 1.2


ART = {"front": ("tau.png", "Tau"), "back": ("kappa.png", "Kappa")}
ART_PX = 0.13   # mm per pixel: 140 px -> 18.2 mm


def pixel_rects(img):
    """Run-length the set pixels per row, then merge identical runs down the rows."""
    w, h = img.size
    px = img.load()
    open_runs, rects = {}, []
    for y in range(h + 1):
        runs = set()
        if y < h:
            x = 0
            while x < w:
                if px[x, y]:
                    x0 = x
                    while x < w and px[x, y]:
                        x += 1
                    runs.add((x0, x))
                else:
                    x += 1
        for r in list(open_runs):
            if r not in runs:
                rects.append((r[0], r[1], open_runs.pop(r), y))
        for r in runs:
            open_runs.setdefault(r, y)
    return rects


def uid():
    return f'(uuid "{uuid.uuid4()}")'


def width(net, in_pin_zone=False):
    if in_pin_zone:
        return W_PIN_ZONE
    return W_PWR if net in POWER else W_SIG


def build(side):
    pins = FRONT_NEEDED if side == "front" else BACK_NEEDED
    hdr_a, hdr_b = HEADERS[side]

    terminals = []   # (kind, x, z, net, ref, padname)
    for n in pins:
        terminals.append(("pogo", pad_x(n), pad_z(n), PIN_NET[n], f"P{n}", "1"))

    # Header rows in the gaps
    hdr_z = None   # set after lanes are known
    hdr_pos = []
    for gap, names in ((GAP_A, hdr_a), (GAP_B, hdr_b)):
        width_needed = (len(names) - 1) * HDR_PITCH
        assert width_needed <= gap[1] - gap[0], f"{side}: {len(names)} headers do not fit {gap}"
        x_start = (gap[0] + gap[1]) / 2 - width_needed / 2
        hdr_pos.append([(x_start + k * HDR_PITCH, net) for k, net in enumerate(names)])

    # VREF divider (front only): R1 VDD-VREF, R2 VREF-VSS, C1 VREF-VSS, in gap A,
    # on a second row offset half a pitch so its tracks pass between header pads.
    divider = []
    if side == "front":
        x = hdr_pos[0][0][0] + HDR_PITCH / 2
        divider = [("R1", x, ("VDD", "VREF"), RES_SPAN),
                   ("R2", x + RES_SPAN + HDR_PITCH, ("VREF", "VSS"), RES_SPAN),
                   ("C1", x + 2 * RES_SPAN + 2 * HDR_PITCH, ("VREF", "VSS"), CAP_SPAN)]

    nets = sorted({t[3] for t in terminals} | {n for row in hdr_pos for _, n in row}
                  | {n for d in divider for n in d[2]},
                  key=lambda n: (n not in POWER, n))

    # Lanes: one per net, stacked downward from LANE_TOP
    lane_z, z, prev_w = {}, LANE_TOP, None
    for net in nets:
        w = width(net)
        if prev_w is not None:
            z -= max(VIA_D / 2 + w / 2, VIA_D / 2 + prev_w / 2, prev_w / 2 + w / 2) + CLEAR + 0.05
        lane_z[net] = z
        prev_w = w
    hdr_z = z - 4.0
    div_z = hdr_z - 8.5          # header labels hang below the header row
    z_bot = div_z - 4.5 if divider else hdr_z - 7.0
    # House rule: every PCB carries a cat. Between the header gaps, below the lanes.
    art_px = Image.open(HERE / "art" / ART[side][0]).convert("1")
    art_top = z - 2.5
    art_bot = art_top - art_px.height * ART_PX
    z_bot = min(z_bot, art_bot - 4.5)

    for row in hdr_pos:
        for k, (x, net) in enumerate(row):
            terminals.append(("hdr", x, hdr_z, net, None, None))
    for ref, x, (na, nb), span in divider:
        terminals.append(("div", x, div_z, na, ref, "1"))
        terminals.append(("div", x + span, div_z, nb, ref, "2"))

    # Sanity: no two vertical tracks closer than pad + clearance where they could meet
    xs = sorted(t[1] for t in terminals if t[0] == "pogo")
    assert all(b - a >= PITCH - 1e-6 for a, b in zip(xs, xs[1:]))
    for t in terminals:
        if t[0] != "pogo":
            assert all(abs(t[1] - xp) > 2.0 for xp in xs), f"{t} too close to a pogo track"
    for bx in BOLT_X:
        assert all(abs(bx - xp) > BOLT_HOLE / 2 + 1.0 for xp in xs), f"bolt {bx} on a pin track"

    # KiCad transform
    xmin, xmax = X0, BOARD_LEN - X0

    OX, OY = 30, 50   # place the board mid-page so a 1:1 print keeps the overlay

    def kx(x):
        return round(OX + ((x - xmin) if side == "front" else (xmax - x)), 4)

    def ky(z):
        return round(OY + Z_TOP - z, 4)

    net_ids = {n: i + 1 for i, n in enumerate(nets)}
    out = []
    out.append('(kicad_pcb (version 20240108) (generator "sint100_gen_pcb") (generator_version "8.0")')
    out.append(' (general (thickness 1.6) (legacy_teardrops no))')
    out.append(' (paper "A4")')
    out.append(' (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (32 "B.Adhes" user "B.Adhesive")'
               ' (33 "F.Adhes" user "F.Adhesive") (34 "B.Paste" user) (35 "F.Paste" user)'
               ' (36 "B.SilkS" user "B.Silkscreen") (37 "F.SilkS" user "F.Silkscreen")'
               ' (38 "B.Mask" user) (39 "F.Mask" user) (40 "Dwgs.User" user "User.Drawings")'
               ' (41 "Cmts.User" user "User.Comments") (44 "Edge.Cuts" user) (45 "Margin" user)'
               ' (46 "B.CrtYd" user "B.Courtyard") (47 "F.CrtYd" user "F.Courtyard")'
               ' (48 "B.Fab" user) (49 "F.Fab" user))')
    out.append(' (setup (pad_to_mask_clearance 0) (allow_soldermask_bridges_in_footprints no))')
    out.append(' (net 0 "")')
    for n, i in net_ids.items():
        out.append(f' (net {i} "{n}")')

    def text(s, x, y, layer="F.SilkS", size=1.0, rot=0, justify=None):
        j = f" (justify {justify})" if justify else ""
        mirror = " mirror" if layer.startswith("B.") else ""
        just = f" (justify{mirror}{(' ' + justify) if justify else ''})" if (mirror or justify) else ""
        out.append(f' (gr_text "{s}" (at {x} {y} {rot}) (layer "{layer}") {uid()}'
                   f' (effects (font (size {size} {size}) (thickness {size * 0.15:.3f})){just}))')

    def footprint(name, ref, x, y, pads, value="", ref_visible=False):
        out.append(f' (footprint "sint100:{name}" (layer "F.Cu") {uid()} (at {x} {y})')
        hide = "" if ref_visible else " (hide yes)"
        out.append(f'  (property "Reference" "{ref}" (at 0 -1.6 0) (layer "F.SilkS") {uid()}{hide}'
                   f' (effects (font (size 0.8 0.8) (thickness 0.12))))')
        out.append(f'  (property "Value" "{value or name}" (at 0 1.6 0) (layer "F.Fab") {uid()} (hide yes)'
                   f' (effects (font (size 0.8 0.8) (thickness 0.12))))')
        out.append('  (attr through_hole)')
        for p in pads:
            out.append("  " + p)
        out.append(' )')

    def pad(num, shape, dx, dy, size, drill, net):
        nn = f' (net {net_ids[net]} "{net}")' if net else ""
        return (f'(pad "{num}" thru_hole {shape} (at {dx} {dy}) (size {size} {size}) (drill {drill})'
                f' (layers "*.Cu" "*.Mask"){nn} {uid()})')

    def seg(x1, z1, x2, z2, w, layer, net):
        out.append(f' (segment (start {kx(x1)} {ky(z1)}) (end {kx(x2)} {ky(z2)}) (width {w})'
                   f' (layer "{layer}") (net {net_ids[net]}) {uid()})')

    def via(x, z, net):
        out.append(f' (via (at {kx(x)} {ky(z)}) (size {VIA_D}) (drill {VIA_DRILL}) (layers "F.Cu" "B.Cu")'
                   f' (net {net_ids[net]}) {uid()})')

    # Board outline
    out.append(f' (gr_rect (start {OX} {OY}) (end {round(OX + xmax - xmin, 4)} {ky(z_bot)})'
               f' (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts") {uid()})')

    # Pogo pins
    for kind, x, z, net, ref, _ in terminals:
        if kind == "pogo":
            footprint("POGO_P75", ref, kx(x), ky(z), [pad("1", "circle", 0, 0, POGO_PAD, POGO_DRILL, net)],
                      value=net)

    # Headers, one footprint per gap row
    for gi, row in enumerate(hdr_pos):
        x0r = row[0][0]
        pads = [pad(str(k + 1), "rect" if k == 0 else "circle",
                    round((kx(x) - kx(x0r)), 4), 0, HDR_PAD, HDR_DRILL, net)
                for k, (x, net) in enumerate(row)]
        footprint(f"HDR_1x{len(row)}", f"J{gi + 1}", kx(x0r), ky(hdr_z), pads, ref_visible=True)
        for x, net in row:
            text(net, kx(x), ky(hdr_z) + 1.4, size=0.8, rot=90, justify="right")

    # Divider parts
    for ref, x, (na, nb), span in divider:
        val = "100n" if ref.startswith("C") else "1k"
        footprint("AXIAL" if ref.startswith("R") else "CAP_THT", ref, kx(x), ky(div_z),
                  [pad("1", "circle", 0, 0, RES_PAD, RES_DRILL, na),
                   pad("2", "circle", round(kx(x + span) - kx(x), 4), 0, RES_PAD, RES_DRILL, nb)],
                  value=val, ref_visible=True)
        text(f"{ref} {val}", kx(x + span / 2), ky(div_z) + 1.8, size=0.8)

    # Bolt holes
    for i, bx in enumerate(BOLT_X):
        out.append(f' (footprint "sint100:M3" (layer "F.Cu") {uid()} (at {kx(bx)} {ky(BOLT_Z)})')
        out.append(f'  (property "Reference" "H{i + 1}" (at 0 0 0) (layer "F.SilkS") {uid()} (hide yes)'
                   f' (effects (font (size 0.8 0.8) (thickness 0.12))))')
        out.append(f'  (property "Value" "M3" (at 0 0 0) (layer "F.Fab") {uid()} (hide yes)'
                   f' (effects (font (size 0.8 0.8) (thickness 0.12))))')
        out.append('  (attr exclude_from_pos_files exclude_from_bom)')
        out.append(f'  (pad "" np_thru_hole circle (at 0 0) (size {BOLT_HOLE} {BOLT_HOLE}) (drill {BOLT_HOLE})'
                   f' (layers "*.Cu" "*.Mask") {uid()})')
        out.append(' )')

    # Routing
    for net in nets:
        lz = lane_z[net]
        members = [t for t in terminals if t[3] == net]
        xs_net = [t[1] for t in members]
        for kind, x, z, _, _, _ in members:
            if kind == "pogo":
                z_zone = min(ROW_Z) - POGO_PAD / 2 - 0.3   # below the pad rows, widen
                seg(x, z, x, z_zone, W_PIN_ZONE, "B.Cu", net)
                seg(x, z_zone, x, lz, width(net), "B.Cu", net)
            elif kind == "div":   # threads between header pads: keep it thin
                seg(x, z, x, lz, W_SIG, "B.Cu", net)
            else:
                seg(x, z, x, lz, width(net), "B.Cu", net)
            via(x, lz, net)
        seg(min(xs_net), lz, max(xs_net), lz, width(net), "F.Cu", net)

    # Cat portrait (one footprint so its pixels don't trip silk-overlap checks)
    cx = kx((GAP_A[1] + GAP_B[0]) / 2)
    x_left = round(cx - art_px.width * ART_PX / 2, 4)
    y_top = ky(art_top)
    out.append(f' (footprint "sint100:ART_{ART[side][1].upper()}" (layer "F.Cu") {uid()} (at {x_left} {y_top})')
    out.append(f'  (property "Reference" "ART{1 if side == "front" else 2}" (at 0 0 0) (layer "F.SilkS") {uid()} (hide yes)'
               f' (effects (font (size 0.8 0.8) (thickness 0.12))))')
    out.append(f'  (property "Value" "{ART[side][1]}" (at 0 0 0) (layer "F.Fab") {uid()} (hide yes)'
               f' (effects (font (size 0.8 0.8) (thickness 0.12))))')
    out.append('  (attr board_only exclude_from_pos_files exclude_from_bom)')
    for x0r, x1r, y0r, y1r in pixel_rects(art_px):
        a, b = round(x0r * ART_PX, 4), round(x1r * ART_PX, 4)
        c, d = round(y0r * ART_PX, 4), round(y1r * ART_PX, 4)
        out.append(f'  (fp_poly (pts (xy {a} {c}) (xy {b} {c}) (xy {b} {d}) (xy {a} {d}))'
                   f' (stroke (width 0) (type solid)) (fill yes) (layer "F.SilkS") {uid()})')
    out.append(' )')
    text(ART[side][1], cx, ky(art_bot) + 1.5, size=1.0)
    text("Designed by Nakomis/C", cx, ky(art_bot) + 3.2, size=1.0)

    # Alignment overlay on User.Drawings (not manufactured): the DIMM's edge, ends,
    # notch and every gold pad, for the 1:1 paper test, plus a 100 mm scale bar.
    def dline(x1, z1, x2, z2, w=0.08):
        out.append(f' (gr_line (start {kx(x1)} {ky(z1)}) (end {kx(x2)} {ky(z2)})'
                   f' (stroke (width {w}) (type default)) (layer "Dwgs.User") {uid()})')
    dline(0, 0, BOARD_LEN, 0, 0.12)
    for xe in (0, BOARD_LEN):
        dline(xe, 0, xe, Z_TOP + 8, 0.12)
    nx = scad_num("notch_x")
    dline(nx - 0.75, 0, nx - 0.75, 3.0); dline(nx + 0.75, 0, nx + 0.75, 3.0); dline(nx - 0.75, 3.0, nx + 0.75, 3.0)
    for i in range(1, 121):
        xp = pad_x(i)
        dline(xp - 0.4, 0.3, xp - 0.4, PAD_TOP); dline(xp + 0.4, 0.3, xp + 0.4, PAD_TOP)
        dline(xp - 0.4, PAD_TOP, xp + 0.4, PAD_TOP)
    out.append(f' (gr_text "DIMM {"front" if side == "front" else "back"} face, seen from outside: edge, ends, notch, pads (pad_top {PAD_TOP} mm)"'
               f' (at {kx(BOARD_LEN / 2)} {ky(Z_TOP + 10)} 0) (layer "Dwgs.User") {uid()}'
               f' (effects (font (size 1.2 1.2) (thickness 0.15))))')
    sb_y = ky(z_bot) + 8
    out.append(f' (gr_line (start {OX + 10} {sb_y}) (end {OX + 110} {sb_y}) (stroke (width 0.2) (type default)) (layer "Dwgs.User") {uid()})')
    for t in range(0, 101, 10):
        out.append(f' (gr_line (start {OX + 10 + t} {sb_y - (1.5 if t % 50 == 0 else 0.8)}) (end {OX + 10 + t} {sb_y})'
                   f' (stroke (width 0.15) (type default)) (layer "Dwgs.User") {uid()})')
    out.append(f' (gr_text "100 mm - measure this to check the print is at 100%" (at {OX + 60} {sb_y + 2.5} 0)'
               f' (layer "Dwgs.User") {uid()} (effects (font (size 1.2 1.2) (thickness 0.15))))')

    # Labels
    title = ("SINT-100 FRONT jaw  pins 1-120" if side == "front" else "SINT-100 BACK jaw  pins 121-240")
    text(title, kx((GAP_A[0] + GAP_A[1]) / 2), ky(Z_TOP) + 1.6, size=1.2)
    end_label = "pin 1 end" if side == "front" else "pin 121 end"
    text(end_label, kx(PIN1_X + 6), ky(Z_TOP) + 1.6, size=0.9)
    text("solder pin tails this side", kx((GAP_B[0] + GAP_B[1]) / 2), ky(Z_TOP) + 1.6, size=0.9)

    out.append(')')
    path = HERE / f"{side}.kicad_pcb"
    path.write_text("\n".join(out) + "\n")
    print(f"{path.name}: {len(nets)} nets, {sum(1 for t in terminals if t[0] == 'pogo')} pogo pins, "
          f"board {xmax - xmin:.2f} x {Z_TOP - z_bot:.2f} mm")


if __name__ == "__main__":
    for s in ("front", "back"):
        build(s)
