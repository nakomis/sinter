"""SN74AVC16T245 test board: a TSSOP-48 -> DIP-48 adapter for the DIMM spike rehearsal.

J1 pin n = U1 pin n (1-24); J2 pin k = U1 pin 49-k, in standard DIP order. Nets are
named after the chip pins, so GND, VCCA and VCCB are each shared by several pins.
"""
from pathlib import Path

from board.netlist import Circuit
from board.parts import part
from board.placement import Placement

# Silkscreen. The board is a DIP-48 adapter, so header ends carry the CHIP pin numbers:
# J1 runs 1-24 left to right, J2 runs 48-25 left to right. U1 also gets a pin 1 dot.
SILK_PIN1 = (("U1", "1", "1"), ("J1", "1", "1"), ("J1", "24", "24"),
             ("J2", "1", "48"), ("J2", "24", "25"))


TITLE = "SINT-101 AVC16T245"
CREDIT = "Designed by Nakomis/C"
# J2 carries chip pins 25-48 (the A side, VCCA); J1 carries 1-24 (the B side, VCCB).
SIDE_TEXT = ("J2 = A side: VCCA 3.3 V", "J1 = B side: VCCB 1.5 V")
ART_PNG = Path(__file__).resolve().parent.parent.parent / "fixture" / "pcb" / "art" / "kappa.png"
ART_HEIGHT = 10.4        # mm: fits between the header rows with 0.5 mm+ to the pads


def art_xlat(placement):
    """(png, x, y, height) for write_board(art=...): Kappa, in the free area left of U1."""
    ux, uy, _ = placement.at["U1"]
    return (ART_PNG, 8.0, uy - ART_HEIGHT / 2, ART_HEIGHT)


def silk_xlat(circuit, placement):
    """Silkscreen entries (text, x, y, size) for the board writer: pin numbers at the header
    ends, then the title, credit and supply labels in the free area right of U1."""
    from board.kicad_pcb import pin_marks
    x0, y0, x1, y1 = placement.outline
    ux, uy, _ = placement.at["U1"]
    right = (ux + 5.75 + x1) / 2 + 0.5
    # J2 is the top row (smaller y), J1 the bottom row.
    out = [(TITLE, right, uy - 4.2, 1.0), (CREDIT, right, uy - 2.6, 1.0),
           (SIDE_TEXT[0], right, uy + 1.0, 1.0), (SIDE_TEXT[1], right, uy + 2.6, 1.0)]
    # Pin numbers go on the inboard side of each header (clear of the header outline); U1's
    # '1' goes to the left of its pin 1 pad.
    pad_x = lambda r, p: placement.at[r][0] + next(
        q.x for q in circuit.instances[r].part.footprint.pads if q.number == p)
    marks = [(r, p, t, (pad_x(r, p), uy)) for r, p, t in SILK_PIN1 if r != "U1"]
    marks += [("U1", "1", "1", (ux - 20.0, uy + 3.71))]       # left of U1 pin 1
    return out + pin_marks(circuit, placement, marks)


ROW_PITCH = 15.24        # 0.6" between header rows
EDGE = 2.5               # keep-out from the board edge
CAP_Y = 5.5              # cap row distance from the chip centre line
CAP_DX = 1.2             # cap centre offset either side of a supply pin's x (0402 courtyard 1.84 mm wide)


def build_xlat_test():
    c = Circuit()
    u = c.add("XLAT", "U1")
    j1 = c.add("HDR_1X24", "J1")
    j2 = c.add("HDR_1X24", "J2")
    names = {pad: name for name, pads in u.part.pins.items() for pad in pads}
    for n in range(1, 25):
        c.connect(names[str(n)], (u, names[str(n)], str(n)), (j1, str(n)))
    for k in range(1, 25):
        pad = str(49 - k)
        c.connect(names[pad], (u, names[pad], pad), (j2, str(k)))
    gnd = names["4"]
    for rail in ("VCCA", "VCCB"):
        for pad in u.part.pins[rail]:
            for key in ("C100N", "C1U"):
                cap = c.add(key)
                c.connect(rail, (cap, "1"))
                c.connect(gnd, (cap, "2"))
    return c


def _supply_pads(circuit):
    """(rail, pad) for every supply pin of U1, in the order build_xlat_test() decouples them."""
    u = circuit.instances["U1"].part
    return [(rail, pad) for rail in ("VCCA", "VCCB") for pad in u.pins[rail]]


def place_xlat_test(circuit):
    """U1 centred between the headers, unrotated: its pin 1 is bottom-left, level with
    J1 pin 1, so pins 1-24 run along J1 and 25-48 along J2. Each supply pin gets a
    100 nF (towards the chip centre) and a 1 uF cap (outwards) either side of it, in the
    strip between the chip and its header."""
    pads = {p.number: p for p in circuit.instances["U1"].part.footprint.pads}
    hdr = circuit.instances["J1"].part.footprint.courtyard
    w = (hdr[2] - hdr[0]) + 2 * EDGE
    h = ROW_PITCH + (hdr[3] - hdr[1]) + 2 * EDGE
    cx, cy = w / 2, h / 2
    at = {"U1": (cx, cy, 0.0),
          "J1": (cx, cy + ROW_PITCH / 2, 0.0),
          "J2": (cx, cy - ROW_PITCH / 2, 0.0)}
    # Caps were added in pairs (100n, 1u) per supply pad, so ref order matches _supply_pads.
    caps = [r for r, i in circuit.instances.items() if i.part.key in ("C100N", "C1U")]
    for n, (_, pad) in enumerate(_supply_pads(circuit)):
        p = pads[pad]
        side = 1 if p.y > 0 else -1               # bottom row (J1 side) or top row (J2 side)
        out = 1 if p.x > 0 else -1                # away from the chip centre line
        c100, c1u = caps[2 * n], caps[2 * n + 1]
        at[c100] = (cx + p.x - out * CAP_DX, cy + side * CAP_Y, 0.0)
        at[c1u] = (cx + p.x + out * CAP_DX, cy + side * CAP_Y, 0.0)
    courtyards = {r: i.part.footprint.courtyard for r, i in circuit.instances.items()}
    return Placement((0.0, 0.0, w, h), at, courtyards)
