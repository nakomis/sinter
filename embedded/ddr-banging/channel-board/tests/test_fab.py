import csv
import zipfile

from board.fab import fab, write_bom, write_cpl
from board.netlist import Circuit
from board.parts import PARTS, ROTATION_CHECKED
from board.placement import Placement
from board.xlat_test import build_xlat_test, place_xlat_test

C = build_xlat_test()
P = place_xlat_test(C)


def _synthetic():
    """A tiny circuit with a DNP instance, as the channel board's lanes 1-7 will be."""
    c = Circuit()
    a, b = c.add("C100N"), c.add("C100N")
    d = c.add("C100N", dnp=True)
    e = c.add("R10K")
    p = Placement((10.0, 20.0, 40.0, 60.0),
                  {a.ref: (15.0, 50.0, 0.0), b.ref: (20.0, 45.0, 90.0),
                   d.ref: (25.0, 40.0, 0.0), e.ref: (30.0, 30.0, 270.0)})
    return c, p, (a.ref, b.ref, d.ref, e.ref)


def test_bom_groups_and_excludes_dnp(tmp_path):
    c, _, (a, b, d, e) = _synthetic()
    write_bom(c, tmp_path / "bom.csv")
    rows = list(csv.DictReader(open(tmp_path / "bom.csv")))
    assert set(rows[0]) == {"Comment", "Designator", "Footprint", "LCSC Part #"}
    refs = [r for row in rows for r in row["Designator"].split(",")]
    assert sorted(refs) == sorted([a, b, e]) and d not in refs
    assert len(rows) == 2
    assert all(row["LCSC Part #"].startswith("C") for row in rows)


def test_test_board_bom_has_every_part_and_no_art(tmp_path):
    write_bom(C, tmp_path / "bom.csv")
    rows = list(csv.DictReader(open(tmp_path / "bom.csv")))
    refs = [r for row in rows for r in row["Designator"].split(",")]
    assert sorted(refs) == sorted(C.instances)
    assert "J1" in refs and "U1" in refs


def test_cpl_rotation_and_coordinates(tmp_path):
    c, p, (a, b, d, e) = _synthetic()
    write_cpl(c, p, tmp_path / "cpl.csv")
    rows = {r["Designator"]: r for r in csv.DictReader(open(tmp_path / "cpl.csv"))}
    assert d not in rows
    for ref in (a, b, e):
        want = (p.at[ref][2] + c.instances[ref].part.rotation_offset) % 360
        assert abs(float(rows[ref]["Rotation"]) - want) < 0.01
        assert rows[ref]["Layer"] == "Top"
    # Gerber frame: x as in KiCad, y negated, whatever the outline origin
    assert abs(float(rows[a]["Mid X"]) - 15.0) < 1e-6
    assert abs(float(rows[a]["Mid Y"]) - -50.0) < 1e-6


def test_cpl_matches_kicad_position_file(tmp_path):
    import subprocess
    from board.fab import KICAD_CLI
    from board.kicad_pcb import write_board
    pcb = tmp_path / "x.kicad_pcb"
    write_board(C, P, pcb, layers=2)
    pos = tmp_path / "pos.csv"
    subprocess.run([KICAD_CLI, "pcb", "export", "pos", "--format", "csv", "--units", "mm",
                    "--side", "front", "-o", str(pos), str(pcb)], check=True, capture_output=True)
    want = {r["Ref"]: r for r in csv.DictReader(open(pos))}
    write_cpl(C, P, tmp_path / "cpl.csv")
    got = {r["Designator"]: r for r in csv.DictReader(open(tmp_path / "cpl.csv"))}
    assert set(got) == set(want)
    for ref, r in got.items():
        assert abs(float(r["Mid X"]) - float(want[ref]["PosX"])) < 0.01, ref
        assert abs(float(r["Mid Y"]) - float(want[ref]["PosY"])) < 0.01, ref


def test_cpl_includes_through_hole_parts(tmp_path):
    write_cpl(C, P, tmp_path / "cpl.csv")
    rows = {r["Designator"] for r in csv.DictReader(open(tmp_path / "cpl.csv"))}
    assert rows == set(C.instances) and {"J1", "J2", "U1"} <= rows


def test_every_fitted_extended_part_has_a_checked_offset():
    for key, p in PARTS.items():
        if p.extended:
            assert key in ROTATION_CHECKED, key


def test_footprint_origins_are_pad_pattern_centres():
    for key, p in PARTS.items():
        pads = [q for q in p.footprint.pads if q.number]
        xs, ys = [q.x for q in pads], [q.y for q in pads]
        assert abs((min(xs) + max(xs)) / 2) < 0.05 and abs((min(ys) + max(ys)) / 2) < 0.05, key


def test_fab_outputs(tmp_path):
    from board.kicad_pcb import write_board
    from board.xlat_test import silk_xlat
    pcb = tmp_path / "xlat-test.kicad_pcb"
    write_board(C, P, pcb, layers=2, silk=silk_xlat(C, P))
    fab(pcb, tmp_path, layers=2)
    names = zipfile.ZipFile(tmp_path / "xlat-test-gerbers.zip").namelist()
    assert any(n.endswith("-F_Cu.gtl") for n in names) and any(n.endswith("-B_Cu.gbl") for n in names)
    assert not any("In1" in n for n in names)
    assert any(n.endswith("-F_Paste.gtp") for n in names)
    assert any(n.endswith("-PTH.drl") for n in names)
    assert (tmp_path / "xlat-test-top.png").stat().st_size > 1000
    assert (tmp_path / "xlat-test-bottom.png").stat().st_size > 1000


def test_stackup_colours(tmp_path):
    from board.kicad_pcb import write_board
    from board.sexpr import find, find_all, parse
    pcb = tmp_path / "x.kicad_pcb"
    write_board(C, P, pcb, layers=2, mask="White", silkscreen="Black")
    st = find(find(parse(pcb.read_text()), "setup"), "stackup")
    col = {l[1]: find(l, "color")[1] for l in find_all(st, "layer") if find(l, "color")}
    assert col == {"F.SilkS": "Black", "B.SilkS": "Black", "F.Mask": "White", "B.Mask": "White"}
