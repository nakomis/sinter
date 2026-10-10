import json
import subprocess

import pytest
from conftest import KICAD_CLI

from board.kicad_pcb import Track, Via, pin1_marks, write_board
from board.netlist import Circuit
from board.placement import Placement
from board.sexpr import find, find_all, parse
from board.xlat_test import SILK_PIN1, SILK_TEXT, build_xlat_test, place_xlat_test

BAD = ("courtyards_overlap", "malformed_courtyard", "invalid_outline", "lib_footprint_issues")


def _silk(c, pl):
    x0, y0, x1, y1 = pl.outline
    texts = [(t, (x0 + x1) / 2, y0 + 1.3 + 1.2 * i, 1.0) for i, t in enumerate(SILK_TEXT)]
    return texts + list(pin1_marks(c, pl, SILK_PIN1))


def _drc(path, out):
    r = subprocess.run([KICAD_CLI, "pcb", "drc", "--format", "json", "--severity-all",
                        "-o", str(out), str(path)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return json.loads(out.read_text())


def test_kicad_parses_unrouted_two_layer_board(tmp_path):
    c = build_xlat_test()
    pl = place_xlat_test(c)
    pcb = tmp_path / "channel.kicad_pcb"
    write_board(c, pl, pcb, layers=2, silk=_silk(c, pl))
    drc = _drc(pcb, tmp_path / "drc.json")
    assert drc["unconnected_items"], "an unrouted board must report unconnected items"
    assert [v for v in drc["violations"] if v["type"] in BAD] == []


def test_four_layer_skips_absent_1v5_zone(tmp_path):
    c = build_xlat_test()
    pcb = tmp_path / "channel.kicad_pcb"
    write_board(c, place_xlat_test(c), pcb, layers=4)
    tree = parse(pcb.read_text())
    zones = find_all(tree, "zone")
    assert [(find(z, "net_name")[1], find(z, "layer")[1]) for z in zones] == [("GND", "In1.Cu")]
    _drc(pcb, tmp_path / "drc.json")


def test_two_layer_zone_and_layers(tmp_path):
    c = build_xlat_test()
    pcb = tmp_path / "channel.kicad_pcb"
    write_board(c, place_xlat_test(c), pcb, layers=2)
    tree = parse(pcb.read_text())
    cu = [l[1] for l in find(tree, "layers")[1:] if l[2] in ("signal", "power")]
    assert cu == ["F.Cu", "B.Cu"]
    z = find_all(tree, "zone")
    assert [(find(x, "net_name")[1], find(x, "layer")[1]) for x in z] == [("GND", "B.Cu")]


def test_bad_layer_count(tmp_path):
    c = build_xlat_test()
    with pytest.raises(ValueError):
        write_board(c, place_xlat_test(c), tmp_path / "x.kicad_pcb", layers=3)


def test_pro_file_read_by_drc(tmp_path):
    """The 0.15 mm default clearance in .kicad_pro must reach DRC: two pads 0.1 mm
    apart violate it, and the report states the 0.15 mm limit."""
    c = Circuit()
    a = c.add("C100N", "C1")
    b = c.add("C100N", "C2")
    c.connect("A", (a, "1"), (b, "1"))
    c.connect("B", (a, "2"), (b, "2"))
    pl = Placement((0.0, 0.0, 10.0, 10.0), {"C1": (3.0, 5.0, 0.0), "C2": (4.45, 5.0, 0.0)})
    pcb = tmp_path / "channel.kicad_pcb"
    write_board(c, pl, pcb, layers=2)
    pro = json.loads((tmp_path / "channel.kicad_pro").read_text())
    assert pro["board"]["design_settings"]["rules"]["min_clearance"] == 0.15
    drc = _drc(pcb, tmp_path / "drc.json")
    clr = [v for v in drc["violations"] if v["type"] == "clearance"]
    assert clr and "0.1500 mm" in clr[0]["description"]


def test_dnp_flag_written(tmp_path):
    c = build_xlat_test()
    pcb = tmp_path / "channel.kicad_pcb"
    write_board(c, place_xlat_test(c), pcb, layers=2)
    assert pcb.read_text().count(" dnp)") == sum(1 for i in c.instances.values() if i.dnp) == 0


def test_dnp_flag_on_synthetic_dnp(tmp_path):
    c = Circuit()
    a = c.add("C100N", "C1", dnp=True)
    b = c.add("C100N", "C2")
    c.connect("A", (a, "1"), (b, "1"))
    pl = Placement((0.0, 0.0, 10.0, 10.0), {"C1": (3.0, 5.0, 0.0), "C2": (7.0, 5.0, 0.0)})
    pcb = tmp_path / "channel.kicad_pcb"
    write_board(c, pl, pcb, layers=2)
    attrs = [find(f, "attr") for f in find_all(parse(pcb.read_text()), "footprint")]
    assert sum(1 for a in attrs if "dnp" in a) == 1


def test_tracks_and_vias_written(tmp_path):
    c = build_xlat_test()
    pcb = tmp_path / "channel.kicad_pcb"
    write_board(c, place_xlat_test(c), pcb, layers=2,
                tracks=[Track("GND", "F.Cu", 5, 5, 8, 5, 0.5)], vias=[Via("GND", 9, 5)])
    tree = parse(pcb.read_text())
    assert len(find_all(tree, "segment")) == 1 and len(find_all(tree, "via")) == 1
    _drc(pcb, tmp_path / "drc.json")
