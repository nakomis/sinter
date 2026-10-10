"""SN74AVC16T245 test board: a TSSOP-48 -> DIP-48 adapter for the DIMM spike rehearsal.

J1 pin n = U1 pin n (1-24); J2 pin k = U1 pin 49-k, in standard DIP order. Nets are
named after the chip pins, so GND, VCCA and VCCB are each shared by several pins.
"""
from board.netlist import Circuit
from board.parts import part
from board.placement import Placement

# Intended silkscreen text, for the board writer (Task 8) to render.
SILK_TEXT = ("SINT-101 AVC16T245", "VCCA = 3.3 V  VCCB = 1.5 V")
# Pin 1 marks: (ref, text) pairs, drawn beside the pin 1 pad of each part.
SILK_PIN1 = (("U1", "1"), ("J1", "1"), ("J2", "1"))

ROW_PITCH = 15.24        # 0.6" between header rows
EDGE = 2.5               # keep-out from the board edge
CAP_Y = 5.5              # cap row distance from the chip centre line
CAP_DX = 0.92            # cap centre offset either side of a supply pin's x (0402 courtyard 1.84 mm wide)


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
