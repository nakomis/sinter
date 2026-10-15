"""Generate the TE 1-1932000-0 DDR3 DIMM socket footprint (EasyEDA has none).

Source: TE customer drawing C-1932000 rev H7, sheet 3 "Recommended PCB layout"
(viewed from connector side, i.e. looking down on the PCB top):
  * 240 x drill 0.70 +-0.08 plated holes, 1.00 mm pitch, four staggered rows
    at y = -2.85, -0.95, +0.95, +2.85 from the centreline (0.95 / 2.85 dims);
  * pin 1 -> 48 = 47.00, 48 -> 49 spans the key (5.00), 49 -> 120 = 71.00;
  * front pins (1-120) odd in the +0.95 row, even in the +2.85 row; back pins
    (121-240) odd in the -2.85 row, even in the -0.95 row; pin 120+n shares x
    with pin n (labels PIN 121/122/1/2 on the drawing);
  * 2 x O2.45 pegs 7.50 mm outside pins 1 and 120; key slot 1.80 x 2.45 at the
    key centre; 142.00 x 7.75 max footprint; extractor shadow areas 11.50 mm
    beyond each end (165.00 max with latches open).
Origin is the centre of the 142 x 7.75 keep-out.

    python -m board.dimm_footprint     # rewrites footprints/lcsc.pretty/DIMM-TH_1-1932000-0.kicad_mod
"""
from pathlib import Path

NAME = "DIMM-TH_1-1932000-0"
OUT = Path(__file__).resolve().parent.parent / "footprints" / "lcsc.pretty" / f"{NAME}.kicad_mod"

DRILL = 0.70
PAD = 1.00                       # drawing allows 0.15 mm max annular ring
ROW_NEAR, ROW_FAR = 0.95, 2.85
KEEPOUT_W, KEEPOUT_H = 142.0, 7.75
PEG_D = 2.45
PEG_OUTSIDE = 7.50               # peg centre to pin 1 / pin 120
KEY_GAP = 5.00
SHADOW = 11.50
KEEPOUT_LEFT = PEG_OUTSIDE + 2.00  # keep-out edge to the left of pin 1 (2X 2.00 from the peg)
KEY_SLOT = (1.80, 2.45)          # width (x), length (y)


def contact_xy(n):
    """(x, y) of contact n relative to pin 1, y down, top view."""
    k = n - 120 if n > 120 else n
    x = (k - 1) if k <= 48 else (k - 1) + KEY_GAP - 1.0
    odd = k % 2 == 1
    if n <= 120:
        y = ROW_NEAR if odd else ROW_FAR
    else:
        y = -ROW_FAR if odd else -ROW_NEAR
    return x, y


def render():
    px = lambda x: x + KEEPOUT_LEFT - KEEPOUT_W / 2   # pin-1-relative x -> footprint x (origin = keep-out centre)
    L = [f'(footprint "{NAME}" (layer "F.Cu")',
         '\t(descr "TE 1-1932000-0 DDR3 DIMM socket, generated from TE drawing C-1932000 rev H7 sheet 3")',
         '\t(attr through_hole)',
         '\t(fp_text reference "REF**" (at 0 -6) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))',
         f'\t(fp_text value "{NAME}" (at 0 6) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))',
         '\t(fp_text user "%R" (at 0 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))',
         '\t(fp_text user "Extractor shadow areas (11.50 mm each end, 165 max open latch): keep clear, see TE C-1932000 sheet 1" '
         '(at 0 4.6) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.1))))']
    hw, hh = KEEPOUT_W / 2, KEEPOUT_H / 2
    L.append(f'\t(fp_rect (start {-hw:.2f} {-hh:.3f}) (end {hw:.2f} {hh:.3f}) (stroke (width 0.05) (type solid)) (fill none) (layer "F.CrtYd"))')
    L.append(f'\t(fp_rect (start {-hw:.2f} {-hh:.3f}) (end {hw:.2f} {hh:.3f}) (stroke (width 0.1) (type solid)) (fill none) (layer "F.Fab"))')
    for sgn in (-1, 1):
        a, b = sgn * hw, sgn * (hw + SHADOW)
        L.append(f'\t(fp_rect (start {a:.2f} {-hh:.3f}) (end {b:.2f} {hh:.3f}) (stroke (width 0.1) (type dash)) (fill none) (layer "F.Fab"))')
    # silkscreen pin-1 mark: triangle left of pin 1, between the end peg and the contacts
    x1, y1 = px(0), ROW_NEAR
    L.append(f'\t(fp_poly (pts (xy {x1 - 1.4:.2f} {y1 - 0.5:.2f}) (xy {x1 - 1.4:.2f} {y1 + 0.5:.2f}) (xy {x1 - 0.7:.2f} {y1:.2f})) '
             '(stroke (width 0.12) (type solid)) (fill solid) (layer "F.SilkS"))')
    L.append(f'\t(fp_text user "1" (at {x1 - 2.4:.2f} {y1:.2f}) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))')
    for n in range(1, 241):
        x, y = contact_xy(n)
        L.append(f'\t(pad "{n}" thru_hole circle (at {px(x):.3f} {y:.3f}) (size {PAD:.2f} {PAD:.2f}) '
                 f'(drill {DRILL:.2f}) (layers "*.Cu" "*.Mask"))')
    for x in (px(-PEG_OUTSIDE), px(123.0 + PEG_OUTSIDE)):
        L.append(f'\t(pad "" np_thru_hole circle (at {x:.3f} 0) (size {PEG_D} {PEG_D}) (drill {PEG_D}) (layers "*.Mask"))')
    kx = px(47.0 + KEY_GAP / 2)
    L.append(f'\t(pad "" np_thru_hole oval (at {kx:.3f} 0) (size {KEY_SLOT[0]} {KEY_SLOT[1]}) '
             f'(drill oval {KEY_SLOT[0]} {KEY_SLOT[1]}) (layers "*.Mask"))')
    L.append(')')
    return "\n".join(L) + "\n"


if __name__ == "__main__":
    OUT.write_text(render())
    print(f"wrote {OUT}")
