import math

from board.netlist import check
from board.placement import Placement, bbox, rotate
from board.xlat_test import SILK_TEXT, build_xlat_test, place_xlat_test

C = build_xlat_test()


def test_clean():
    assert check(C) == []


def test_dip_mapping():
    for n in range(1, 25):
        assert C.net_of("J1", str(n)) == C.net_of("U1", str(n)), n
    for k in range(1, 25):
        assert C.net_of("J2", str(k)) == C.net_of("U1", str(49 - k)), k


def test_every_supply_pin_decoupled():
    u = C.instances["U1"].part
    for rail in ("VCCA", "VCCB"):
        caps = [r for r, i in C.instances.items() if i.part.key in ("C100N", "C1U")
                and rail in (C.net_of(r, "1"), C.net_of(r, "2"))]
        assert len(caps) >= 2 * len(u.pins[rail]), rail


def test_placement_inside_outline():
    P = place_xlat_test(C)
    x0, y0, x1, y1 = P.outline
    for ref in C.instances:
        a = bbox(P, ref)
        assert a[0] >= x0 and a[1] >= y0 and a[2] <= x1 and a[3] <= y1, ref
        # global constraint: components at least 2.5 mm from the board edge
        assert a[0] - x0 >= 2.5 and a[1] - y0 >= 2.5 and x1 - a[2] >= 2.5 and y1 - a[3] >= 2.5, ref


def _pad_xy(P, ref, pad):
    p = next(q for q in C.instances[ref].part.footprint.pads if q.number == pad)
    x, y, rot = P.at[ref]
    dx, dy = rotate(p.x, p.y, rot)
    return x + dx, y + dy


def test_caps_within_2mm_of_their_supply_pin():
    P = place_xlat_test(C)
    u = C.instances["U1"].part
    for rail in ("VCCA", "VCCB"):
        pins = [_pad_xy(P, "U1", pad) for pad in u.pins[rail]]
        for key in ("C100N", "C1U"):
            caps = [r for r, i in C.instances.items() if i.part.key == key and C.net_of(r, "1") == rail]
            for pin in pins:       # each supply pin has its own cap of each kind nearby
                assert any(min(math.dist(pin, _pad_xy(P, r, "1")), math.dist(pin, _pad_xy(P, r, "2"))) <= 2.0
                           for r in caps), (rail, key, pin)


def test_headers_parallel_and_dip_spaced():
    P = place_xlat_test(C)
    (x1, y1), (x2, y2) = _pad_xy(P, "J1", "1"), _pad_xy(P, "J2", "1")
    assert abs(x1 - x2) < 1e-6 and abs(abs(y1 - y2) - 15.24) < 1e-6
    # pin 1 of U1 sits at the J1 pin 1 end, pin 48 at the J2 pin 1 end
    assert abs(_pad_xy(P, "U1", "1")[0] - _pad_xy(P, "U1", "48")[0]) < 1e-6
    assert _pad_xy(P, "U1", "1")[0] < _pad_xy(P, "U1", "24")[0]
    assert _pad_xy(P, "U1", "1")[1] > P.at["U1"][1] > _pad_xy(P, "U1", "48")[1]
    assert _pad_xy(P, "J1", "1")[1] > _pad_xy(P, "J2", "1")[1]


def test_no_courtyard_overlaps():
    P = place_xlat_test(C)
    refs = list(C.instances)
    for i, a in enumerate(refs):
        for b in refs[i + 1:]:
            A, B = bbox(P, a), bbox(P, b)
            assert A[2] <= B[0] + 1e-9 or B[2] <= A[0] + 1e-9 or A[3] <= B[1] + 1e-9 or B[3] <= A[1] + 1e-9, (a, b)


def test_bbox_rotation_and_manual_placement():
    P = Placement((0, 0, 10, 10), {"X": (5, 5, 90)}, {"X": (-2.0, -1.0, 2.0, 1.0)})
    x0, y0, x1, y1 = bbox(P, "X")
    assert (round(x0, 6), round(y0, 6), round(x1, 6), round(y1, 6)) == (4.0, 3.0, 6.0, 7.0)
    assert Placement((0, 0, 1, 1), {}).courtyards == {}


def test_silk_text():
    assert "SINT-101 AVC16T245" in SILK_TEXT
    assert any("3.3" in t and "1.5" in t for t in SILK_TEXT)
